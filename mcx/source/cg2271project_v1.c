/*
 * CG2271 Assignment 4 — MCXC444 FreeRTOS firmware (stream 3)
 *
 * Receives game state / events from the ESP32 over UART2, drives three
 * actuators from them, polls a light sensor and reports it back.
 *
 * Hardware (pins checked against MCXC444 RM ch.10 + FRDM schematic):
 *   UART2 TX   PTE22  ALT4        -> ESP32 RX      9600 8N1
 *   UART2 RX   PTE23  ALT4        <- ESP32 TX
 *   Red LED    PTE31  GPIO        onboard, active-LOW
 *   Servo      PTA5   ALT3        TPM0_CH2, 50 Hz          (header D5)
 *   Buzzer     PTA12  ALT3        TPM1_CH0, passive, tone  (header D3)
 *   LDR        PTB2   ALT0        ADC0_SE12                (header A2)
 *              wiring: 3V3 - LDR - A2 - 10k - GND  (bright = high reading)
 *
 * Protocol: see protocol.h ($GS / $EV in, $ACK / $NAK / $LDR out).
 *
 * RTOS design:
 *   ISRs (non-blocking, FromISR APIs only):
 *     UART2_FLEXIO_IRQHandler  RX: frame lines -> rxQueue
 *                              TX: send txBuf char-by-char -> give txDoneSem
 *     ADC0_IRQHandler          conversion done -> notify ldrTask
 *   Tasks:
 *     commRxTask   pri 3   parse frames, update GameState, dispatch, ACK/NAK
 *     commTxTask   pri 2   sole owner of UART TX (txQueue -> ISR)
 *     ldrTask      pri 2   polled LDR every 100 ms, report to ESP32
 *     ledTask      pri 1   LED patterns           } time-sliced with
 *     buzzerTask   pri 1   tone sequences         } each other
 *     servoTask    pri 1   lives gauge            }
 *   Sync:
 *     Queues   rxQueue, txQueue, ledQueue, buzzerQueue
 *     Mutex    gameStateMutex (GameState), printMutex (debug console)
 *     Binary semaphore  txDoneSem (ISR -> commTxTask)
 *     Task notification ADC ISR -> ldrTask
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_debug_console.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "protocol.h"

/* ===================================================================
 * Pin & peripheral constants
 * =================================================================*/
#define BAUD_RATE            9600U
#define UART_TX_PTE22        22U
#define UART_RX_PTE23        23U
#define UART2_INT_PRIO       1U     /* 0 = highest on CM0+ (2 prio bits) */
#define ADC0_INT_PRIO        2U

#define RED_PIN              31U    /* PTE31 onboard active-LOW red LED  */
#define SERVO_PIN             5U    /* PTA5  TPM0_CH2                    */
#define SERVO_CH              2U
#define BUZZER_PIN           12U    /* PTA12 TPM1_CH0                    */
#define BUZZER_CH             0U
#define LDR_PIN               2U    /* PTB2  ADC0_SE12                   */
#define LDR_ADC_CH           12U

/* TPM clock: IRC48M (TPMSRC = 1).
 * Servo  TPM0: 48 MHz / 128 = 375 kHz, MOD 7499 -> 50 Hz, 1 count = 2.67 us.
 * Buzzer TPM1: 48 MHz / 16  = 3 MHz,   MOD = 3 MHz / f - 1.               */
#define SERVO_MOD          7499U
#define SERVO_MIN_CNV       225U    /* 0.6 ms -> 0 deg   (tune if servo buzzes) */
#define SERVO_MAX_CNV       900U    /* 2.4 ms -> 180 deg                       */
#define BUZZER_TPM_HZ   3000000UL

/* ===================================================================
 * Application tuning
 * =================================================================*/
#define MAX_MSG_LEN          (PROTO_MAX_LINE + 2U)
#define RX_QUEUE_LEN          5U
#define TX_QUEUE_LEN          6U
#define TX_FRAME_LEN         32U
#define TX_TIMEOUT_MS       200U    /* 32 chars @ 9600 baud ~ 33 ms */

#define LINK_TIMEOUT_MS    2000U    /* no valid frame for this long -> link lost */

#define LDR_PERIOD_MS       100U
#define LDR_REPORT_EVERY      2U    /* send $LDR every 2nd sample = 200 ms */
#define LDR_AVG_LEN           4U
#define LDR_DARK_BELOW     1200U    /* hysteresis thresholds (calibrate!)   */
#define LDR_LIGHT_ABOVE    1600U

#define LED_TICK_MS          50U
#define LED_FLASH_MS        100U
#define SERVO_PERIOD_MS      50U
#define SERVO_SLEW_DEG        6U    /* max change per period -> smooth sweep */
#define GAUGE_MAX_LIVES       3     /* lives that map to 180 deg            */

/* ===================================================================
 * Types
 * =================================================================*/
typedef struct { char message[MAX_MSG_LEN]; } TMessage;
typedef struct { char text[TX_FRAME_LEN]; } TxFrame;

typedef enum { LED_FLASH_HIT = 0, LED_FLASH_BONUS } LedCmd;

typedef struct {
    int32_t     lives;
    int32_t     score;
    game_mode_t mode;
    bool        linkUp;
} GameState;

typedef struct { uint16_t freq; uint16_t ms; } Note;   /* freq 0 = rest */

/* ===================================================================
 * FreeRTOS objects & shared data
 * =================================================================*/
static QueueHandle_t      rxQueue;         /* UART ISR   -> commRxTask    */
static QueueHandle_t      txQueue;         /* any task   -> commTxTask    */
static QueueHandle_t      ledQueue;        /* commRxTask -> ledTask       */
static QueueHandle_t      buzzerQueue;     /* commRxTask -> buzzerTask    */
static SemaphoreHandle_t  gameStateMutex;  /* protects gameState          */
static SemaphoreHandle_t  printMutex;      /* protects the debug console  */
static SemaphoreHandle_t  txDoneSem;       /* UART ISR   -> commTxTask    */
static TaskHandle_t       ldrTaskHandle;   /* ADC ISR notifies this task  */

static GameState gameState = { 0, 0, MODE_IDLE, false };

/* TX buffer: written by commTxTask only while TIE is off, read by the
 * ISR only while TIE is on, so the txDoneSem handshake protects it. */
static char              txBuf[TX_FRAME_LEN];
static volatile uint32_t txIdx;
static volatile uint16_t ldrRaw;

volatile uint32_t uartDroppedMessages = 0;
volatile uint32_t txDroppedFrames = 0;

#define LOG(...) do {                                                   \
        if (xSemaphoreTake(printMutex, pdMS_TO_TICKS(50)) == pdTRUE) {  \
            PRINTF(__VA_ARGS__);                                        \
            xSemaphoreGive(printMutex);                                 \
        }                                                               \
    } while (0)

static const char *const EVENT_NAMES[] = { "HIT", "START", "OVER", "BONUS" };
static const char *const MODE_NAMES[]  = { "IDLE", "PLAY", "OVER" };

/* ===================================================================
 * Low-level: LED
 * =================================================================*/
static void initLED(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTE_MASK;
    GPIOE->PSOR  = 1UL << RED_PIN;           /* latch HIGH -> LED off */
    GPIOE->PDDR |= 1UL << RED_PIN;           /* output                */
    PORTE->PCR[RED_PIN] = PORT_PCR_MUX(1U);  /* GPIO                  */
}

static inline void setRedLED(bool on)
{
    if (on) GPIOE->PCOR = 1UL << RED_PIN;    /* LOW  = on (active-LOW) */
    else    GPIOE->PSOR = 1UL << RED_PIN;    /* HIGH = off             */
}

/* ===================================================================
 * Low-level: TPM0 servo + TPM1 buzzer (edge-aligned PWM)
 * =================================================================*/
static void initPWM(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK;
    SIM->SCGC6 |= SIM_SCGC6_TPM0_MASK | SIM_SCGC6_TPM1_MASK;

    PORTA->PCR[SERVO_PIN]  = PORT_PCR_MUX(3U);   /* TPM0_CH2 */
    PORTA->PCR[BUZZER_PIN] = PORT_PCR_MUX(3U);   /* TPM1_CH0 */

    SIM->SOPT2 &= ~SIM_SOPT2_TPMSRC_MASK;
    SIM->SOPT2 |=  SIM_SOPT2_TPMSRC(1U);         /* IRC48M */

    /* Servo: 50 Hz, start centred. */
    TPM0->SC  = 0;
    TPM0->CNT = 0;
    TPM0->MOD = SERVO_MOD;
    TPM0->CONTROLS[SERVO_CH].CnSC = TPM_CnSC_MSB_MASK | TPM_CnSC_ELSB_MASK;
    TPM0->CONTROLS[SERVO_CH].CnV  = (SERVO_MIN_CNV + SERVO_MAX_CNV) / 2U;
    TPM0->SC  = TPM_SC_CMOD(1U) | TPM_SC_PS(7U);  /* /128 */

    /* Buzzer: silent (0 % duty) until a tone is requested. */
    TPM1->SC  = 0;
    TPM1->CNT = 0;
    TPM1->MOD = (uint32_t)(BUZZER_TPM_HZ / 1000UL) - 1U;
    TPM1->CONTROLS[BUZZER_CH].CnSC = TPM_CnSC_MSB_MASK | TPM_CnSC_ELSB_MASK;
    TPM1->CONTROLS[BUZZER_CH].CnV  = 0;
    TPM1->SC  = TPM_SC_CMOD(1U) | TPM_SC_PS(4U);  /* /16 */
}

/* 0 .. 180 degrees. */
static void setServoDeg(uint32_t deg)
{
    if (deg > 180U) deg = 180U;
    TPM0->CONTROLS[SERVO_CH].CnV = SERVO_MIN_CNV + deg * (SERVO_MAX_CNV - SERVO_MIN_CNV) / 180U;
}

/* Square wave at freq Hz (50 % duty); freq 0 silences the buzzer. */
static void setBuzzerTone(uint32_t freq)
{
    if (freq == 0U) {
        TPM1->CONTROLS[BUZZER_CH].CnV = 0;
        return;
    }
    uint32_t mod = BUZZER_TPM_HZ / freq - 1U;
    TPM1->MOD = mod;
    TPM1->CNT = 0;                                 /* avoid a long first period */
    TPM1->CONTROLS[BUZZER_CH].CnV = (mod + 1U) / 2U;
}

/* ===================================================================
 * Low-level: ADC0 (12-bit, software trigger, completion interrupt)
 * =================================================================*/
static void initADC(void)
{
    NVIC_DisableIRQ(ADC0_IRQn);
    SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK;
    SIM->SCGC6 |= SIM_SCGC6_ADC0_MASK;
    PORTB->PCR[LDR_PIN] = PORT_PCR_MUX(0U);       /* analog */

    ADC0->CFG1 = ADC_CFG1_ADIV(2U)                /* bus 24 MHz / 4 = 6 MHz */
               | ADC_CFG1_ADLSMP_MASK             /* long sample: high-Z divider */
               | ADC_CFG1_MODE(1U);               /* 12-bit single-ended    */
    ADC0->CFG2 = 0;
    ADC0->SC2  = ADC_SC2_REFSEL(1U);              /* VALTH/VALTL = VDDA, software trigger */
    ADC0->SC3  = ADC_SC3_AVGE_MASK | ADC_SC3_AVGS(1U);  /* hw average of 8, single shot */

    NVIC_SetPriority(ADC0_IRQn, ADC0_INT_PRIO);
    NVIC_ClearPendingIRQ(ADC0_IRQn);
    NVIC_EnableIRQ(ADC0_IRQn);
}

static inline void startADC(uint32_t channel)
{
    ADC0->SC1[0] = ADC_SC1_AIEN_MASK | ADC_SC1_ADCH(channel);
}

/* ===================================================================
 * Low-level: UART2 (ISR-driven RX and TX, as in Assignment 3)
 * =================================================================*/
static void initUART2(uint32_t baud_rate)
{
    NVIC_DisableIRQ(UART2_FLEXIO_IRQn);
    SIM->SCGC4 |= SIM_SCGC4_UART2_MASK;
    SIM->SCGC5 |= SIM_SCGC5_PORTE_MASK;

    UART2->C2 = 0;

    PORTE->PCR[UART_TX_PTE22] = PORT_PCR_MUX(4U);
    PORTE->PCR[UART_RX_PTE23] = PORT_PCR_MUX(4U);

    uint32_t bus_clk = CLOCK_GetBusClkFreq();
    uint32_t sbr = (bus_clk + baud_rate * 8U) / (baud_rate * 16U);
    UART2->BDH = (uint8_t)((sbr >> 8) & UART_BDH_SBR_MASK);
    UART2->BDL = (uint8_t)sbr;
    UART2->C1  = 0;
    UART2->C3  = 0;
    UART2->C4  = 0;
    (void)UART2->S1;
    (void)UART2->D;   /* clear stale flags */

    /* TX stays enabled; TIE is only switched on while a frame is sending. */
    UART2->C2 = UART_C2_RE_MASK | UART_C2_RIE_MASK | UART_C2_TE_MASK;

    NVIC_SetPriority(UART2_FLEXIO_IRQn, UART2_INT_PRIO);
    NVIC_ClearPendingIRQ(UART2_FLEXIO_IRQn);
    NVIC_EnableIRQ(UART2_FLEXIO_IRQn);
}

/* Start an interrupt-driven send. Only commTxTask calls this. */
static void uartStartSend(const char *s)
{
    strncpy(txBuf, s, TX_FRAME_LEN - 1U);
    txBuf[TX_FRAME_LEN - 1U] = '\0';
    txIdx = 0;
    UART2->C2 |= UART_C2_TIE_MASK;   /* TDRE is set, so the ISR fires now */
}

/* ===================================================================
 * ISRs
 * =================================================================*/
void UART2_FLEXIO_IRQHandler(void)
{
    static TMessage msg;
    static uint32_t recv_ptr = 0;
    static bool discard = false;
    BaseType_t hpw = pdFALSE;

    uint8_t status = UART2->S1;
    const uint8_t errors = UART_S1_OR_MASK | UART_S1_NF_MASK
                         | UART_S1_FE_MASK | UART_S1_PF_MASK;

    /* ---- Receive: assemble a line, hand complete lines to commRxTask ---- */
    if (status & (UART_S1_RDRF_MASK | errors)) {
        char c = (char)UART2->D;   /* reading D clears RDRF and error flags */

        if (status & errors) {
            recv_ptr = 0;
            discard = true;
        }

        if (c == '\n') {
            if (!discard && recv_ptr != 0U) {
                msg.message[recv_ptr] = '\0';
                if (xQueueSendFromISR(rxQueue, &msg, &hpw) != pdTRUE)
                    uartDroppedMessages++;
            }
            recv_ptr = 0;
            discard  = false;
        } else if (c != '\r' && !discard) {
            if (recv_ptr < MAX_MSG_LEN - 1U) {
                msg.message[recv_ptr++] = c;
            } else {
                recv_ptr = 0;
                discard  = true;
                uartDroppedMessages++;
            }
        }
    }

    /* ---- Transmit: one char per TDRE while TIE is on ---- */
    if ((UART2->C2 & UART_C2_TIE_MASK) && (UART2->S1 & UART_S1_TDRE_MASK)) {
        if (txBuf[txIdx] != '\0') {
            UART2->D = (uint8_t)txBuf[txIdx++];
        } else {
            UART2->C2 &= ~UART_C2_TIE_MASK;
            xSemaphoreGiveFromISR(txDoneSem, &hpw);
        }
    }

    portYIELD_FROM_ISR(hpw);
}

void ADC0_IRQHandler(void)
{
    BaseType_t hpw = pdFALSE;

    if (ADC0->SC1[0] & ADC_SC1_COCO_MASK) {
        ldrRaw = (uint16_t)ADC0->R[0];             /* reading R clears COCO */
        vTaskNotifyGiveFromISR(ldrTaskHandle, &hpw);
    }
    portYIELD_FROM_ISR(hpw);
}

/* ===================================================================
 * Helpers
 * =================================================================*/
static void queueFrame(const TxFrame *f)
{
    if (xQueueSend(txQueue, f, pdMS_TO_TICKS(20)) != pdTRUE)
        txDroppedFrames++;
}

static void setLinkUp(bool up)
{
    if (xSemaphoreTake(gameStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        gameState.linkUp = up;
        xSemaphoreGive(gameStateMutex);
    }
}

/* ===================================================================
 * TASK: commRxTask (priority 3)
 * Validates frames, ACKs/NAKs every one, updates GameState and dispatches
 * events to the actuator tasks. Pre-empts every other task on arrival.
 * =================================================================*/
static void commRxTask(void *p)
{
    (void)p;
    TMessage line;
    proto_msg_t msg;
    int32_t seq;
    TxFrame reply;
    bool linkUp = false;

    /* Enable UART only once the scheduler and queues are running. */
    initUART2(BAUD_RATE);

    while (1) {
        if (xQueueReceive(rxQueue, &line, pdMS_TO_TICKS(LINK_TIMEOUT_MS)) != pdTRUE) {
            if (linkUp) {
                linkUp = false;
                setLinkUp(false);
                LOG("LINK: lost (no frame for %u ms)\r\n", LINK_TIMEOUT_MS);
            }
            continue;
        }

        proto_status_t st = proto_parse(line.message, &msg, &seq);
        if (st != PROTO_OK) {
            proto_build(reply.text, sizeof reply.text, "NAK,%u", (unsigned)(seq < 0 ? 0 : seq));
            queueFrame(&reply);
            LOG("RX: rejected (err %d): %s\r\n", (int)st, line.message);
            continue;
        }

        proto_build(reply.text, sizeof reply.text, "ACK,%u", (unsigned)msg.seq);
        queueFrame(&reply);

        if (!linkUp) {
            linkUp = true;
            LOG("LINK: up\r\n");
        }

        if (msg.type == MSG_GAME_STATE) {
            game_mode_t oldMode = MODE_IDLE;
            if (xSemaphoreTake(gameStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                oldMode          = gameState.mode;
                gameState.lives  = msg.u.gs.lives;
                gameState.score  = msg.u.gs.score;
                gameState.mode   = msg.u.gs.mode;
                gameState.linkUp = true;
                xSemaphoreGive(gameStateMutex);
            }
            if (oldMode != msg.u.gs.mode)
                LOG("STATE: %s lives=%ld score=%ld\r\n", MODE_NAMES[msg.u.gs.mode],
                    (long)msg.u.gs.lives, (long)msg.u.gs.score);
        } else {
            game_event_t ev = msg.u.ev;
            LOG("EVENT: %s\r\n", EVENT_NAMES[ev]);

            /* Events that change the mode take effect before the next $GS. */
            if (ev == EV_START || ev == EV_OVER) {
                if (xSemaphoreTake(gameStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    gameState.mode   = (ev == EV_START) ? MODE_PLAY : MODE_OVER;
                    gameState.linkUp = true;
                    xSemaphoreGive(gameStateMutex);
                }
            }
            if (ev == EV_HIT || ev == EV_BONUS) {
                LedCmd cmd = (ev == EV_HIT) ? LED_FLASH_HIT : LED_FLASH_BONUS;
                xQueueSend(ledQueue, &cmd, 0);
            }
            xQueueSend(buzzerQueue, &ev, 0);    /* every event has a sound */
        }
    }
}

/* ===================================================================
 * TASK: commTxTask (priority 2)
 * Sole owner of UART2 TX: takes frames from txQueue, starts the ISR-driven
 * send and blocks on txDoneSem until the ISR has queued the last byte.
 * =================================================================*/
static void commTxTask(void *p)
{
    (void)p;
    TxFrame f;

    while (1) {
        if (xQueueReceive(txQueue, &f, portMAX_DELAY) != pdTRUE)
            continue;

        uartStartSend(f.text);
        if (xSemaphoreTake(txDoneSem, pdMS_TO_TICKS(TX_TIMEOUT_MS)) != pdTRUE) {
            UART2->C2 &= ~UART_C2_TIE_MASK;     /* abandon this frame */
            txDroppedFrames++;
            LOG("TX: timeout\r\n");
        }
    }
}

/* ===================================================================
 * TASK: ldrTask (priority 2)
 * Polls the LDR every LDR_PERIOD_MS: starts a conversion and sleeps until
 * the ADC ISR notifies it. Smooths the value, applies hysteresis, and
 * reports to the ESP32 periodically and immediately on a dark/light change.
 * =================================================================*/
static void ldrTask(void *p)
{
    (void)p;
    uint16_t hist[LDR_AVG_LEN] = { 0 };
    uint32_t histIdx = 0, sinceReport = 0;
    bool primed = false, dark = false;
    uint16_t seq = 0;
    TxFrame f;
    TickType_t lastWake = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(LDR_PERIOD_MS));

        (void)ulTaskNotifyTake(pdTRUE, 0);              /* drop any stale notify */
        startADC(LDR_ADC_CH);
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)) == 0U) {
            LOG("LDR: ADC timeout\r\n");
            continue;
        }
        uint16_t raw = ldrRaw;

        if (!primed) {                                   /* fill on first sample */
            for (uint32_t i = 0; i < LDR_AVG_LEN; i++) hist[i] = raw;
            dark = raw < LDR_DARK_BELOW;
            primed = true;
        }
        hist[histIdx] = raw;
        histIdx = (histIdx + 1U) % LDR_AVG_LEN;

        uint32_t sum = 0;
        for (uint32_t i = 0; i < LDR_AVG_LEN; i++) sum += hist[i];
        uint16_t avg = (uint16_t)(sum / LDR_AVG_LEN);

        bool changed = false;
        if (!dark && avg < LDR_DARK_BELOW)        { dark = true;  changed = true; }
        else if (dark && avg > LDR_LIGHT_ABOVE)   { dark = false; changed = true; }

        if (changed || ++sinceReport >= LDR_REPORT_EVERY) {
            sinceReport = 0;
            proto_build(f.text, sizeof f.text, "LDR,%u,%u,%u",
                        (unsigned)seq++, (unsigned)avg, dark ? 1U : 0U);
            queueFrame(&f);
            if (changed) LOG("LDR: %s (avg %u)\r\n", dark ? "dark" : "light", (unsigned)avg);
        }
    }
}

/* ===================================================================
 * TASK: ledTask (priority 1)
 * Flash patterns on HIT / BONUS; otherwise shows mode / link state:
 *   link lost  1 Hz slow blink     PLAY  solid on
 *   OVER       5 Hz fast blink     IDLE  short blip every 2 s
 * =================================================================*/
static void flashLED(uint32_t times)
{
    for (uint32_t i = 0; i < times; i++) {
        setRedLED(true);
        vTaskDelay(pdMS_TO_TICKS(LED_FLASH_MS));
        setRedLED(false);
        vTaskDelay(pdMS_TO_TICKS(LED_FLASH_MS));
    }
}

static void ledTask(void *p)
{
    (void)p;
    LedCmd cmd;

    while (1) {
        if (xQueueReceive(ledQueue, &cmd, pdMS_TO_TICKS(LED_TICK_MS)) == pdTRUE) {
            flashLED(cmd == LED_FLASH_HIT ? 3U : 2U);
            continue;
        }

        game_mode_t mode = MODE_IDLE;
        bool link = false;
        if (xSemaphoreTake(gameStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            mode = gameState.mode;
            link = gameState.linkUp;
            xSemaphoreGive(gameStateMutex);
        }

        uint32_t ms = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
        bool on;
        if (!link)                  on = (ms % 1000U) < 500U;
        else if (mode == MODE_OVER) on = (ms % 200U)  < 100U;
        else if (mode == MODE_PLAY) on = true;
        else                        on = (ms % 2000U) < 100U;
        setRedLED(on);
    }
}

/* ===================================================================
 * TASK: buzzerTask (priority 1)
 * Plays a short tone sequence per game event on the passive buzzer.
 * The tone itself is hardware PWM (TPM1), so timing jitter does not
 * affect pitch; the task only sequences notes.
 * =================================================================*/
static const Note TUNE_HIT[]   = { {392, 80}, {262, 160}, {0, 0} };
static const Note TUNE_START[] = { {523, 100}, {659, 100}, {784, 160}, {0, 0} };
static const Note TUNE_OVER[]  = { {784, 200}, {523, 200}, {392, 200}, {262, 400}, {0, 0} };
static const Note TUNE_BONUS[] = { {784, 80}, {1047, 120}, {0, 0} };
static const Note *const TUNES[] = { TUNE_HIT, TUNE_START, TUNE_OVER, TUNE_BONUS };

static void buzzerTask(void *p)
{
    (void)p;
    game_event_t ev;

    while (1) {
        if (xQueueReceive(buzzerQueue, &ev, portMAX_DELAY) != pdTRUE)
            continue;

        for (const Note *n = TUNES[ev]; n->ms != 0U; n++) {
            setBuzzerTone(n->freq);
            vTaskDelay(pdMS_TO_TICKS(n->ms));
        }
        setBuzzerTone(0);
        vTaskDelay(pdMS_TO_TICKS(30));                  /* gap between tunes */
    }
}

/* ===================================================================
 * TASK: servoTask (priority 1)
 * Lives gauge: 0 lives = 0 deg, GAUGE_MAX_LIVES = 180 deg, slewed for a
 * smooth sweep. Reads GameState under the mutex every SERVO_PERIOD_MS.
 * =================================================================*/
static void servoTask(void *p)
{
    (void)p;
    uint32_t current = 90U;
    TickType_t lastWake = xTaskGetTickCount();

    while (1) {
        int32_t lives = 0;
        game_mode_t mode = MODE_IDLE;
        if (xSemaphoreTake(gameStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            lives = gameState.lives;
            mode  = gameState.mode;
            xSemaphoreGive(gameStateMutex);
        }

        if (lives > GAUGE_MAX_LIVES) lives = GAUGE_MAX_LIVES;
        uint32_t target = (mode == MODE_OVER) ? 0U
                        : (uint32_t)lives * 180U / (uint32_t)GAUGE_MAX_LIVES;

        if (current + SERVO_SLEW_DEG < target)       current += SERVO_SLEW_DEG;
        else if (current > target + SERVO_SLEW_DEG)  current -= SERVO_SLEW_DEG;
        else                                         current = target;
        setServoDeg(current);

        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SERVO_PERIOD_MS));
    }
}

/* ===================================================================
 * FreeRTOS hooks: fail loudly (solid red LED) instead of corrupting memory
 * =================================================================*/
static void fatalError(void)
{
    taskDISABLE_INTERRUPTS();
    setRedLED(true);
    setBuzzerTone(0);
    while (1) {}
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    fatalError();
}

void vApplicationMallocFailedHook(void)
{
    fatalError();
}

/* ===================================================================
 * main
 * =================================================================*/
int main(void)
{
    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    BOARD_InitBootPeripherals();
#ifndef BOARD_INIT_DEBUG_CONSOLE_PERIPHERAL
    BOARD_InitDebugConsole();
#endif

    /* Hardware init (UART2 is deferred to commRxTask). */
    initLED();
    initPWM();

    /* ---- Create FreeRTOS objects ---- */
    rxQueue        = xQueueCreate(RX_QUEUE_LEN, sizeof(TMessage));
    txQueue        = xQueueCreate(TX_QUEUE_LEN, sizeof(TxFrame));
    ledQueue       = xQueueCreate(2, sizeof(LedCmd));
    buzzerQueue    = xQueueCreate(4, sizeof(game_event_t));
    gameStateMutex = xSemaphoreCreateMutex();
    printMutex     = xSemaphoreCreateMutex();
    txDoneSem      = xSemaphoreCreateBinary();

    if (!rxQueue || !txQueue || !ledQueue || !buzzerQueue
        || !gameStateMutex || !printMutex || !txDoneSem) {
        PRINTF("ERROR: RTOS object allocation failed\r\n");
        fatalError();
    }

    /* ---- Create tasks ----
     *  commRxTask  pri 3  highest: never miss / delay incoming frames
     *  commTxTask  pri 2  replies go out promptly
     *  ldrTask     pri 2  periodic sensor sampling
     *  led/buzzer/servo pri 1  actuators, time-sliced
     */
    BaseType_t ok = pdPASS;
    ok &= xTaskCreate(commRxTask, "commRx", configMINIMAL_STACK_SIZE + 200, NULL, 3, NULL);
    ok &= xTaskCreate(commTxTask, "commTx", configMINIMAL_STACK_SIZE + 80,  NULL, 2, NULL);
    ok &= xTaskCreate(ldrTask,    "ldr",    configMINIMAL_STACK_SIZE + 200, NULL, 2, &ldrTaskHandle);
    ok &= xTaskCreate(ledTask,    "led",    configMINIMAL_STACK_SIZE + 40,  NULL, 1, NULL);
    ok &= xTaskCreate(buzzerTask, "buzzer", configMINIMAL_STACK_SIZE + 40,  NULL, 1, NULL);
    ok &= xTaskCreate(servoTask,  "servo",  configMINIMAL_STACK_SIZE + 40,  NULL, 1, NULL);

    if (ok != pdPASS) {
        PRINTF("ERROR: task allocation failed\r\n");
        fatalError();
    }

    /* ADC interrupt needs ldrTaskHandle, so enable it after task creation. */
    initADC();

    PRINTF("=== CG2271 A4 MCXC444 controller ===\r\n");
    PRINTF("UART2 %u baud PTE22/23 | LED PTE31 | servo PTA5 | buzzer PTA12 | LDR PTB2\r\n",
           BAUD_RATE);

    vTaskStartScheduler();

    PRINTF("ERROR: scheduler could not start\r\n");
    fatalError();
    return 0;
}
