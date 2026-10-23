/*
 * CG2271 Assignment 4 — MCXC444 FreeRTOS firmware (stream 3)
 *
 * Receives game state / events from the ESP32 over UART2, drives three
 * actuators from them, reads a rotary encoder (throttle) and reports it back.
 *
 * Hardware (pins checked against MCXC444 RM ch.10 + FRDM schematic):
 *   UART2 TX     PTE22  ALT4      -> ESP32 RX      9600 8N1
 *   UART2 RX     PTE23  ALT4      <- ESP32 TX
 *   Red LED      PTE31  GPIO      onboard, active-LOW
 *   Servo        PTA5   ALT3      TPM0_CH2, 50 Hz          (header D5)
 *   Buzzer       PTD4   GPIO      active, active-HIGH      (header D10)
 *   Encoder CLK  PTA12  GPIO      pull-up, IRQ either edge (header D3)
 *   Encoder DT   PTA13  GPIO      pull-up, IRQ either edge (header D8)
 *                module + -> 3V3, GND -> GND, SW unused
 *
 * Protocol: see protocol.h ($GS / $EV in, $ACK / $NAK / $ENC out).
 *
 * RTOS design:
 *   ISRs (non-blocking, FromISR APIs only):
 *     UART2_FLEXIO_IRQHandler  RX: frame lines -> rxQueue
 *                              TX: send txBuf char-by-char -> give txDoneSem
 *     PORTA_IRQHandler         encoder pin change -> decode -> notify encoderTask
 *   Tasks:
 *     commRxTask   pri 3   parse frames, update GameState, dispatch, ACK/NAK
 *     commTxTask   pri 2   sole owner of UART TX (txQueue -> ISR)
 *     encoderTask  pri 2   report throttle level to ESP32 on change + 500 ms
 *     ledTask      pri 1   LED patterns           } time-sliced with
 *     buzzerTask   pri 1   beep patterns          } each other
 *     servoTask    pri 1   lives gauge            }
 *   Sync:
 *     Queues   rxQueue, txQueue, ledQueue, buzzerQueue
 *     Mutex    gameStateMutex (GameState), printMutex (debug console)
 *     Binary semaphore  txDoneSem (ISR -> commTxTask)
 *     Task notification PORTA ISR -> encoderTask
 *     Critical section  encoder state shared by PORTA ISR and encoderTask
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

#include "encoder.h"
#include "protocol.h"

/* ===================================================================
 * Pin & peripheral constants
 * =================================================================*/
#define BAUD_RATE            9600U
#define UART_TX_PTE22        22U
#define UART_RX_PTE23        23U
#define UART2_INT_PRIO       1U     /* 0 = highest on CM0+ (2 prio bits) */
#define PORTA_INT_PRIO       2U

#define RED_PIN              31U    /* PTE31 onboard active-LOW red LED  */
#define HAS_SERVO             0     /* set to 1 once the SG90 is wired to PTA5 */
#define SERVO_PIN             5U    /* PTA5  TPM0_CH2                    */
#define SERVO_CH              2U
#define BUZZER_PIN            4U    /* PTD4  GPIO, active buzzer         */
#define BUZZER_ACTIVE_LOW     0     /* set to 1 if LOW makes it beep     */
#define ENC_CLK_PIN          12U    /* PTA12 GPIO, encoder CLK (D3)      */
#define ENC_DT_PIN           13U    /* PTA13 GPIO, encoder DT  (D8)      */
#define ENC_REVERSE           false /* true if turning right lowers it   */

/* Servo TPM0 clock: IRC48M (TPMSRC = 1) / 128 = 375 kHz,
 * MOD 7499 -> 50 Hz, 1 count = 2.67 us. */
#define SERVO_MOD          7499U
#define SERVO_MIN_CNV       225U    /* 0.6 ms -> 0 deg   (tune if servo buzzes) */
#define SERVO_MAX_CNV       900U    /* 2.4 ms -> 180 deg                       */

/* ===================================================================
 * Application tuning
 * =================================================================*/
#define MAX_MSG_LEN          (PROTO_MAX_LINE + 2U)
#define RX_QUEUE_LEN          5U
#define TX_QUEUE_LEN          6U
#define TX_FRAME_LEN         32U
#define TX_TIMEOUT_MS       200U    /* 32 chars @ 9600 baud ~ 33 ms */

#define LINK_TIMEOUT_MS    2000U    /* no valid frame for this long -> link lost */

#define ENC_HEARTBEAT_MS    500U    /* resend $ENC at least this often       */

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

typedef struct { bool on; uint16_t ms; } Beep;          /* ms 0 = end */

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
static TaskHandle_t       encoderTaskHandle; /* PORTA ISR notifies this task */

static GameState gameState = { 0, 0, MODE_IDLE, false };

/* TX buffer: written by commTxTask only while TIE is off, read by the
 * ISR only while TIE is on, so the txDoneSem handshake protects it. */
static char              txBuf[TX_FRAME_LEN];
static volatile uint32_t txIdx;
/* Encoder decoder state: written by PORTA ISR, read by encoderTask in a
 * critical section. */
static enc_t             encoder;

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
 * Low-level: Buzzer (active buzzer, GPIO on/off)
 * =================================================================*/
static void initBuzzer(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTD_MASK;
#if BUZZER_ACTIVE_LOW
    GPIOD->PSOR  = 1UL << BUZZER_PIN;            /* start HIGH (off)  */
#else
    GPIOD->PCOR  = 1UL << BUZZER_PIN;            /* start LOW (off)   */
#endif
    GPIOD->PDDR |= 1UL << BUZZER_PIN;            /* output            */
    PORTD->PCR[BUZZER_PIN] = PORT_PCR_MUX(1U);   /* GPIO              */
}

static inline void setBuzzer(bool on)
{
    if (on != (bool)BUZZER_ACTIVE_LOW) GPIOD->PSOR = 1UL << BUZZER_PIN;
    else                               GPIOD->PCOR = 1UL << BUZZER_PIN;
}

#if HAS_SERVO
/* ===================================================================
 * Low-level: Servo (TPM0 channel 2, edge-aligned PWM, 50 Hz)
 * =================================================================*/
static void initServo(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK;
    SIM->SCGC6 |= SIM_SCGC6_TPM0_MASK;

    PORTA->PCR[SERVO_PIN] = PORT_PCR_MUX(3U);    /* TPM0_CH2 */

    SIM->SOPT2 &= ~SIM_SOPT2_TPMSRC_MASK;
    SIM->SOPT2 |=  SIM_SOPT2_TPMSRC(1U);         /* IRC48M */

    /* 50 Hz, start centred. */
    TPM0->SC  = 0;
    TPM0->CNT = 0;
    TPM0->MOD = SERVO_MOD;
    TPM0->CONTROLS[SERVO_CH].CnSC = TPM_CnSC_MSB_MASK | TPM_CnSC_ELSB_MASK;
    TPM0->CONTROLS[SERVO_CH].CnV  = (SERVO_MIN_CNV + SERVO_MAX_CNV) / 2U;
    TPM0->SC  = TPM_SC_CMOD(1U) | TPM_SC_PS(7U);  /* /128 */
}

/* 0 .. 180 degrees. */
static void setServoDeg(uint32_t deg)
{
    if (deg > 180U) deg = 180U;
    TPM0->CONTROLS[SERVO_CH].CnV = SERVO_MIN_CNV + deg * (SERVO_MAX_CNV - SERVO_MIN_CNV) / 180U;
}
#endif /* HAS_SERVO */

/* ===================================================================
 * Low-level: rotary encoder (PTA12 CLK / PTA13 DT, pin-change interrupt)
 * =================================================================*/
static inline uint8_t encoderPins(void)
{
    uint32_t in = GPIOA->PDIR;
    return (uint8_t)((((in >> ENC_CLK_PIN) & 1U) << 1) | ((in >> ENC_DT_PIN) & 1U));
}

static void initEncoder(void)
{
    NVIC_DisableIRQ(PORTA_IRQn);
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK;

    /* GPIO, pull-up (module outputs are open/pulled to 3V3), IRQ either edge. */
    const uint32_t pcr = PORT_PCR_MUX(1U) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK
                       | PORT_PCR_IRQC(0xBU);
    PORTA->PCR[ENC_CLK_PIN] = pcr | PORT_PCR_ISF_MASK;
    PORTA->PCR[ENC_DT_PIN]  = pcr | PORT_PCR_ISF_MASK;
    GPIOA->PDDR &= ~((1UL << ENC_CLK_PIN) | (1UL << ENC_DT_PIN));

    encoder_init(&encoder, encoderPins(), ENC_REVERSE);   /* latch idle state */

    PORTA->ISFR = (1UL << ENC_CLK_PIN) | (1UL << ENC_DT_PIN);
    NVIC_SetPriority(PORTA_IRQn, PORTA_INT_PRIO);
    NVIC_ClearPendingIRQ(PORTA_IRQn);
    NVIC_EnableIRQ(PORTA_IRQn);
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

void PORTA_IRQHandler(void)
{
    BaseType_t hpw = pdFALSE;
    const uint32_t mask = (1UL << ENC_CLK_PIN) | (1UL << ENC_DT_PIN);

    if (PORTA->ISFR & mask) {
        PORTA->ISFR = mask;                         /* write-1-to-clear */
        if (encoder_update(&encoder, encoderPins()))
            vTaskNotifyGiveFromISR(encoderTaskHandle, &hpw);
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
    proto_history_t history = { 0 };

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
            setLinkUp(true);
            LOG("LINK: up\r\n");
        }

        /* A lost ACK causes an identical stop-and-wait retry. ACK it again,
         * but keep HIT/BONUS sounds and START/OVER effects one-shot. */
        if (!proto_remember(&history, line.message)) continue;

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
 * TASK: encoderTask (priority 2)
 * Interrupt-driven sensor: the PORTA ISR decodes every pin change and
 * notifies this task when the throttle level moves. The task reports the
 * level to the ESP32 immediately on change, and every ENC_HEARTBEAT_MS
 * otherwise so the ESP32 can tell the reading is still fresh.
 * =================================================================*/
static void encoderTask(void *p)
{
    (void)p;
    uint16_t seq = 0;
    int lastLevel = -1;
    TxFrame f;

    while (1) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ENC_HEARTBEAT_MS));

        taskENTER_CRITICAL();                  /* shared with PORTA ISR */
        int level = encoder.level;
        uint32_t skipped = encoder.skipped;
        taskEXIT_CRITICAL();

        proto_build(f.text, sizeof f.text, "ENC,%u,%u", (unsigned)seq++, (unsigned)level);
        queueFrame(&f);

        if (level != lastLevel) {
            LOG("ENC: throttle %d%% (skipped %lu)\r\n", level, (unsigned long)skipped);
            lastLevel = level;
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
 * Plays an on/off beep pattern per game event on the active buzzer
 * (the buzzer generates its own tone; the task only times the beeps).
 * =================================================================*/
static const Beep PAT_HIT[]   = { {true, 80}, {false, 60}, {true, 80}, {false, 0} };
static const Beep PAT_START[] = { {true, 60}, {false, 80}, {true, 120}, {false, 80},
                                  {true, 200}, {false, 0} };
static const Beep PAT_OVER[]  = { {true, 800}, {false, 0} };
static const Beep PAT_BONUS[] = { {true, 30}, {false, 30}, {true, 30}, {false, 0} };
static const Beep *const PATTERNS[] = { PAT_HIT, PAT_START, PAT_OVER, PAT_BONUS };

static void buzzerTask(void *p)
{
    (void)p;
    game_event_t ev;

    while (1) {
        if (xQueueReceive(buzzerQueue, &ev, portMAX_DELAY) != pdTRUE)
            continue;

        for (const Beep *b = PATTERNS[ev]; b->ms != 0U; b++) {
            setBuzzer(b->on);
            vTaskDelay(pdMS_TO_TICKS(b->ms));
        }
        setBuzzer(false);
        vTaskDelay(pdMS_TO_TICKS(100));                 /* gap between patterns */
    }
}

#if HAS_SERVO
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
#endif /* HAS_SERVO */

/* ===================================================================
 * FreeRTOS hooks: fail loudly (solid red LED) instead of corrupting memory
 * =================================================================*/
static void fatalError(void)
{
    taskDISABLE_INTERRUPTS();
    setRedLED(true);
    setBuzzer(false);
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
    initBuzzer();
#if HAS_SERVO
    initServo();
#endif

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
     *  encoderTask pri 2  sensor reports
     *  led/buzzer/servo pri 1  actuators, time-sliced
     */
    BaseType_t ok = pdPASS;
    ok &= xTaskCreate(commRxTask,  "commRx",  configMINIMAL_STACK_SIZE + 200, NULL, 3, NULL);
    ok &= xTaskCreate(commTxTask,  "commTx",  configMINIMAL_STACK_SIZE + 80,  NULL, 2, NULL);
    ok &= xTaskCreate(encoderTask, "encoder", configMINIMAL_STACK_SIZE + 200, NULL, 2, &encoderTaskHandle);
    ok &= xTaskCreate(ledTask,    "led",    configMINIMAL_STACK_SIZE + 40,  NULL, 1, NULL);
    ok &= xTaskCreate(buzzerTask, "buzzer", configMINIMAL_STACK_SIZE + 40,  NULL, 1, NULL);
#if HAS_SERVO
    ok &= xTaskCreate(servoTask,  "servo",  configMINIMAL_STACK_SIZE + 40,  NULL, 1, NULL);
#endif

    if (ok != pdPASS) {
        PRINTF("ERROR: task allocation failed\r\n");
        fatalError();
    }

    /* The PORTA ISR needs encoderTaskHandle, so enable it after task creation. */
    initEncoder();

    PRINTF("=== CG2271 A4 MCXC444 controller ===\r\n");
    PRINTF("UART2 %u baud PTE22/23 | LED PTE31 | servo PTA5%s | buzzer PTD4 | encoder PTA12/13\r\n",
           BAUD_RATE, HAS_SERVO ? "" : " (disabled)");

    vTaskStartScheduler();

    PRINTF("ERROR: scheduler could not start\r\n");
    fatalError();
    return 0;
}
