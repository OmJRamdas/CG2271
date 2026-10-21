/*
 * SUPERSEDED: replaced by mcx/source/ (cg2271project_v1.c + protocol.c/.h).
 * Kept for reference only. Its servo pin (PTD0, not on any header; the servo
 * is now on PTA5) and its GAME:/SENSOR: protocol are outdated. Buzzer PTD4
 * and PTB0 (now the LDR) are still used. See README "MCXC444 wiring" and
 * "UART Protocol".
 *
 * CG2271 Assignment 4 — MCXC444 Flight Game Controller
 * Replace the application .c file in your existing FreeRTOS project.
 *
 * Hardware wiring:
 *   UART2 TX  PTE22  →  ESP32 RX (GPIO2)     9600 baud
 *   UART2 RX  PTE23  ←  ESP32 TX (GPIO1)     9600 baud
 *   Red LED   PTE31  (onboard, active-LOW)
 *   Buzzer    PTD4   (GPIO toggle, ~500 Hz)
 *   Servo     PTD0   (TPM0_CH0 PWM, 50 Hz)
 *   Pot       PTB0   (ADC0_SE8, 12-bit)
 *
 * Protocol (over UART):
 *   RX from ESP32:  GAME:<event>,<value>\n
 *       HIT,1       → flash red LED
 *       BOOST,1/0   → buzzer beep on/off
 *       ROLL,<deg>  → servo follows plane roll angle
 *       SCORE,<n>   → stored in shared state
 *   TX to ESP32:    SENSOR:POT,<0..4095>\n
 *
 * FreeRTOS (5 tasks, 3 queues, 1 mutex):
 *   communicationTask   pri 3   UART RX parse → dispatch to queues
 *   potTask             pri 2   ADC read → UART TX (every 50 ms)
 *   ledTask             pri 1   flash LED on HIT events
 *   buzzerTask          pri 1   beep on BOOST events
 *   servoTask           pri 1   poll shared state → set servo angle
 *
 * Demonstrates:
 *   - Mutex         : gameStateMutex protects shared GameState struct
 *   - Queues        : uartQueue, ledQueue, buzzerQueue
 *   - Bi-dir UART   : RX ISR (events), blocking TX (pot sensor)
 *   - Time-slicing  : led, buzzer, servo all at priority 1
 *   - Pre-emption   : communicationTask (pri 3) pre-empts actuator tasks
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_debug_console.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* ===================================================================
 * Pin & peripheral constants
 * =================================================================*/
#define BAUD_RATE            9600U
#define UART_TX_PTE22        22U
#define UART_RX_PTE23        23U

#define RED_PIN              31U   /* PTE31  onboard active-LOW red LED */
#define BUZZER_PIN            4U   /* PTD4   passive buzzer             */
#define SERVO_PIN             0U   /* PTD0   SG90 servo (TPM0_CH0)      */
#define POT_PIN               0U   /* PTB0   potentiometer              */
#define POT_ADC_CH            8U   /* ADC0_SE8 for PTB0                 */

/* Servo PWM: TPM0 at 48 MHz / 128 = 375 kHz, MOD 7499 → 50 Hz.
 * 1.0 ms (0°) = 375,  1.5 ms (90°) = 562,  2.0 ms (180°) = 750.  */
#define SERVO_MOD          7499U
#define SERVO_MIN           375U   /* 1.0 ms pulse → 0°   */
#define SERVO_MID           562U   /* 1.5 ms pulse → 90°  */
#define SERVO_MAX           750U   /* 2.0 ms pulse → 180° */

#define MAX_MSG_LEN          64U
#define RX_QUEUE_LEN          5U
#define LED_FLASH_MS        150U
#define BUZZER_BEEP_MS      250U
#define POT_INTERVAL_MS      50U
#define SERVO_POLL_MS        50U

/* ===================================================================
 * Types
 * =================================================================*/
typedef struct { char message[MAX_MSG_LEN]; } TMessage;

typedef struct {
    int32_t  score;
    int32_t  rollAngle;   /* −180 … +180 degrees from game */
} GameState;

/* ===================================================================
 * FreeRTOS objects
 * =================================================================*/
static QueueHandle_t      uartQueue;       /* ISR → communicationTask   */
static QueueHandle_t      ledQueue;        /* comm → ledTask  (bool)    */
static QueueHandle_t      buzzerQueue;     /* comm → buzzerTask (bool)  */
static SemaphoreHandle_t  gameStateMutex;  /* protects gameState struct */

static GameState  gameState = { 0, 0 };
volatile uint32_t uartDroppedMessages = 0;

/* ===================================================================
 * Low-level: LED
 * =================================================================*/
static void initLED(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTE_MASK;
    GPIOE->PSOR  = 1UL << RED_PIN;           /* latch HIGH → LED off   */
    GPIOE->PDDR |= 1UL << RED_PIN;           /* output                 */
    PORTE->PCR[RED_PIN] = PORT_PCR_MUX(1U);  /* GPIO                   */
}

static inline void setRedLED(bool on)
{
    if (on) GPIOE->PCOR = 1UL << RED_PIN;    /* LOW  = on  (active-LOW) */
    else    GPIOE->PSOR = 1UL << RED_PIN;    /* HIGH = off              */
}

/* ===================================================================
 * Low-level: Buzzer (GPIO toggle for ~500 Hz tone)
 * =================================================================*/
static void initBuzzer(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTD_MASK;
    GPIOD->PCOR  = 1UL << BUZZER_PIN;            /* start LOW (off)   */
    GPIOD->PDDR |= 1UL << BUZZER_PIN;            /* output            */
    PORTD->PCR[BUZZER_PIN] = PORT_PCR_MUX(1U);   /* GPIO              */
}

/* ===================================================================
 * Low-level: Servo (TPM0 channel 0, edge-aligned PWM, 50 Hz)
 * =================================================================*/
static void initServo(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTD_MASK;
    SIM->SCGC6 |= SIM_SCGC6_TPM0_MASK;

    PORTD->PCR[SERVO_PIN] = PORT_PCR_MUX(4U);  /* ALT4 = TPM0_CH0 */

    /* TPM clock source: MCGFLLCLK (48 MHz) */
    SIM->SOPT2 &= ~SIM_SOPT2_TPMSRC_MASK;
    SIM->SOPT2 |=  SIM_SOPT2_TPMSRC(1U);

    TPM0->SC   = 0;                             /* stop timer       */
    TPM0->MOD  = SERVO_MOD;                     /* 50 Hz period     */
    TPM0->CONTROLS[0].CnSC = TPM_CnSC_MSB_MASK
                            | TPM_CnSC_ELSB_MASK;  /* edge-aligned PWM */
    TPM0->CONTROLS[0].CnV  = SERVO_MID;         /* centre (90°)     */
    TPM0->SC   = TPM_SC_CMOD(1U)
               | TPM_SC_PS(7U);                  /* prescaler 128    */
}

/* Map roll angle (−90 … +90) to servo pulse CnV (375 … 750). */
static void setServoAngle(int32_t rollDeg)
{
    if (rollDeg < -90) rollDeg = -90;
    if (rollDeg >  90) rollDeg =  90;
    uint32_t cnv = SERVO_MIN
                 + (uint32_t)((rollDeg + 90) * (int32_t)(SERVO_MAX - SERVO_MIN))
                   / 180U;
    TPM0->CONTROLS[0].CnV = cnv;
}

/* ===================================================================
 * Low-level: ADC (12-bit, single-conversion polling)
 * =================================================================*/
static void initADC(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK;
    SIM->SCGC6 |= SIM_SCGC6_ADC0_MASK;
    PORTB->PCR[POT_PIN] &= ~PORT_PCR_MUX_MASK;   /* MUX 0 = analog */
    ADC0->CFG1 = ADC_CFG1_MODE(1U);               /* 12-bit         */
    ADC0->SC3  = 0;                                /* single, no avg */
}

static uint16_t readADC(uint8_t channel)
{
    ADC0->SC1[0] = ADC_SC1_ADCH(channel);
    while (!(ADC0->SC1[0] & ADC_SC1_COCO_MASK)) {}
    return (uint16_t)ADC0->R[0];
}

/* ===================================================================
 * Low-level: UART2 (same ISR-driven RX as Assignment 3, plus TX)
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

    UART2->C2 = UART_C2_RE_MASK | UART_C2_RIE_MASK | UART_C2_TE_MASK;

    NVIC_SetPriority(UART2_FLEXIO_IRQn, (1U << __NVIC_PRIO_BITS) - 1U);
    NVIC_ClearPendingIRQ(UART2_FLEXIO_IRQn);
    NVIC_EnableIRQ(UART2_FLEXIO_IRQn);
}

/* Blocking TX — safe because only potTask calls it. */
static void uartTxByte(uint8_t c)
{
    while (!(UART2->S1 & UART_S1_TDRE_MASK)) {}
    UART2->D = c;
}

static void uartTxString(const char *s)
{
    while (*s) uartTxByte((uint8_t)*s++);
}

/* ===================================================================
 * UART2 RX ISR — line-buffered into uartQueue (same pattern as A3)
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

    if (status & (UART_S1_RDRF_MASK | errors)) {
        char c = (char)UART2->D;

        if (status & errors) {
            recv_ptr = 0;
            discard = true;
        }

        if (c == '\n') {
            if (!discard && recv_ptr != 0U) {
                msg.message[recv_ptr] = '\0';
                if (xQueueSendFromISR(uartQueue, &msg, &hpw) != pdTRUE)
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
    portYIELD_FROM_ISR(hpw);
}

/* ===================================================================
 * Protocol parser: GAME:<event>,<value>
 * =================================================================*/
static bool parseGame(const char *s, char *event, size_t evLen, int32_t *value)
{
    if (strncmp(s, "GAME:", 5) != 0) return false;
    s += 5;

    /* Copy event name until ',' */
    size_t i = 0;
    while (*s && *s != ',' && i < evLen - 1) event[i++] = *s++;
    event[i] = '\0';
    if (*s != ',') return false;
    s++;

    /* Parse signed integer value */
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE) return false;
    *value = (int32_t)v;
    return true;
}

/* ===================================================================
 * TASK: communicationTask (priority 3)
 * Reads UART RX queue, parses GAME: protocol, dispatches to actuators.
 * Pre-empts all actuator tasks.
 * =================================================================*/
static void communicationTask(void *p)
{
    (void)p;
    TMessage msg;
    char event[16];
    int32_t value;

    /* Enable UART after scheduler and queues are running. */
    initUART2(BAUD_RATE);

    while (1) {
        if (xQueueReceive(uartQueue, &msg, portMAX_DELAY) != pdTRUE)
            continue;

        if (!parseGame(msg.message, event, sizeof(event), &value))
            continue;

        if (strcmp(event, "HIT") == 0) {
            /* HIT event → tell ledTask to flash */
            bool flash = true;
            xQueueOverwrite(ledQueue, &flash);
            PRINTF("EVENT: HIT\r\n");
        }
        else if (strcmp(event, "BOOST") == 0) {
            /* BOOST event → tell buzzerTask to beep */
            bool beep = (value != 0);
            xQueueOverwrite(buzzerQueue, &beep);
            PRINTF("EVENT: BOOST %ld\r\n", (long)value);
        }
        else if (strcmp(event, "ROLL") == 0) {
            /* ROLL angle → update shared state under mutex */
            if (xSemaphoreTake(gameStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                gameState.rollAngle = value;
                xSemaphoreGive(gameStateMutex);
            }
        }
        else if (strcmp(event, "SCORE") == 0) {
            /* SCORE → update shared state under mutex */
            if (xSemaphoreTake(gameStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                gameState.score = value;
                xSemaphoreGive(gameStateMutex);
            }
            PRINTF("EVENT: SCORE %ld\r\n", (long)value);
        }
    }
}

/* ===================================================================
 * TASK: potTask (priority 2)
 * Reads potentiometer ADC every 50 ms, sends SENSOR:POT,<val>\n
 * over UART TX to the ESP32.
 * =================================================================*/
static void potTask(void *p)
{
    (void)p;
    char buf[32];
    TickType_t xLastWake = xTaskGetTickCount();

    while (1) {
        uint16_t raw = readADC(POT_ADC_CH);

        snprintf(buf, sizeof(buf), "SENSOR:POT,%u\n", (unsigned)raw);
        uartTxString(buf);

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(POT_INTERVAL_MS));
    }
}

/* ===================================================================
 * TASK: ledTask (priority 1)
 * Flashes red LED three times quickly when a HIT event arrives.
 * Time-slices with buzzerTask and servoTask.
 * =================================================================*/
static void ledTask(void *p)
{
    (void)p;
    bool flash;

    while (1) {
        if (xQueueReceive(ledQueue, &flash, portMAX_DELAY) == pdTRUE
            && flash) {
            /* Triple-flash pattern */
            for (int i = 0; i < 3; i++) {
                setRedLED(true);
                vTaskDelay(pdMS_TO_TICKS(LED_FLASH_MS));
                setRedLED(false);
                vTaskDelay(pdMS_TO_TICKS(LED_FLASH_MS));
            }
        }
    }
}

/* ===================================================================
 * TASK: buzzerTask (priority 1)
 * Generates a ~500 Hz tone for BUZZER_BEEP_MS when BOOST arrives.
 * Uses vTaskDelay(1) per half-cycle ≈ 500 Hz with 1 ms tick.
 * Time-slices with ledTask and servoTask.
 * =================================================================*/
static void buzzerTask(void *p)
{
    (void)p;
    bool beep;

    while (1) {
        if (xQueueReceive(buzzerQueue, &beep, portMAX_DELAY) == pdTRUE
            && beep) {
            /* Tone: toggle pin every 1 ms → ~500 Hz for BUZZER_BEEP_MS */
            uint32_t cycles = BUZZER_BEEP_MS;  /* half-cycles ≈ ms */
            for (uint32_t i = 0; i < cycles; i++) {
                GPIOD->PTOR = 1UL << BUZZER_PIN;   /* toggle */
                vTaskDelay(1);                       /* 1 tick ≈ 1 ms */
            }
            GPIOD->PCOR = 1UL << BUZZER_PIN;        /* ensure off */
        }
    }
}

/* ===================================================================
 * TASK: servoTask (priority 1)
 * Polls shared gameState under mutex every SERVO_POLL_MS and
 * positions the servo to mirror the plane's roll angle.
 * Time-slices with ledTask and buzzerTask.
 * =================================================================*/
static void servoTask(void *p)
{
    (void)p;
    TickType_t xLastWake = xTaskGetTickCount();

    while (1) {
        int32_t roll = 0;

        if (xSemaphoreTake(gameStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            roll = gameState.rollAngle;
            xSemaphoreGive(gameStateMutex);
        }

        setServoAngle(roll);

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(SERVO_POLL_MS));
    }
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

    /* Hardware init (UART2 is deferred to communicationTask). */
    initLED();
    initBuzzer();
    initServo();
    initADC();

    /* ---- Create FreeRTOS objects ---- */
    uartQueue      = xQueueCreate(RX_QUEUE_LEN, sizeof(TMessage));
    ledQueue       = xQueueCreate(1, sizeof(bool));
    buzzerQueue    = xQueueCreate(1, sizeof(bool));
    gameStateMutex = xSemaphoreCreateMutex();

    if (!uartQueue || !ledQueue || !buzzerQueue || !gameStateMutex) {
        PRINTF("ERROR: RTOS object allocation failed\r\n");
        while (1) {}
    }

    /* ---- Create tasks ----
     *  communicationTask  pri 3  (highest — pre-empts actuator tasks)
     *  potTask            pri 2  (sensor polling)
     *  ledTask            pri 1  (actuator — time-slices with peers)
     *  buzzerTask         pri 1  (actuator — time-slices with peers)
     *  servoTask          pri 1  (actuator — time-slices with peers)
     */
    BaseType_t ok = pdPASS;
    ok &= xTaskCreate(communicationTask, "comm",
                       configMINIMAL_STACK_SIZE + 256, NULL, 3, NULL);
    ok &= xTaskCreate(potTask, "pot",
                       configMINIMAL_STACK_SIZE + 128, NULL, 2, NULL);
    ok &= xTaskCreate(ledTask, "led",
                       configMINIMAL_STACK_SIZE + 64,  NULL, 1, NULL);
    ok &= xTaskCreate(buzzerTask, "buzzer",
                       configMINIMAL_STACK_SIZE + 64,  NULL, 1, NULL);
    ok &= xTaskCreate(servoTask, "servo",
                       configMINIMAL_STACK_SIZE + 64,  NULL, 1, NULL);

    if (ok != pdPASS) {
        PRINTF("ERROR: task allocation failed\r\n");
        while (1) {}
    }

    PRINTF("=== CG2271 A4 Flight Controller ===\r\n");
    PRINTF("UART 9600 | LED PTE31 | Buzzer PTD4 | Servo PTD0 | Pot PTB0\r\n");

    vTaskStartScheduler();

    PRINTF("ERROR: scheduler could not start\r\n");
    while (1) {}
}
