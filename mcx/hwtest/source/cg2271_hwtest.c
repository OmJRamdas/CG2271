/*
 * CG2271 A4 — MCXC444 hardware test
 *
 * Standalone check that every MCX sensor/actuator and the UART link to the
 * ESP32 work. All results go to the MCUXpresso debug console (115200).
 *
 *   [LED]   red LED on 1 s                      PTE31 onboard, active-LOW
 *   [BUZ]   3 beeps                             PTD4  (D10), active-HIGH
 *   [SERVO] 0 -> 90 -> 180 -> 90 deg, 1 s each  PTA5  (D5),  TPM0_CH2
 *           (actuator sequence runs at boot and again on every SW3 press)
 *   [ENC]   throttle level on every detent      CLK PTA12 (D3), DT PTA13 (D8)
 *           + edge/skip summary every 5 s       pin-change interrupt
 *   [LINK]  echoes every UART line as ECHO:<line>   UART2 PTE22/PTE23, 9600
 *           (pair with esp32/hw_test/hw_test.ino, which sends PING,<n>)
 *
 * Drivers are copied from mcx/source/cg2271project_v1.c (same pins).
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

/* ===================================================================
 * Pins (identical to the main firmware)
 * =================================================================*/
#define BAUD_RATE            9600U
#define UART_TX_PTE22        22U
#define UART_RX_PTE23        23U
#define UART2_INT_PRIO       1U
#define PORTA_INT_PRIO       2U

#define RED_PIN              31U    /* PTE31 */
#define HAS_SERVO             0     /* set to 1 once the SG90 is wired to PTA5 */
#define SERVO_PIN             5U    /* PTA5  TPM0_CH2 */
#define SERVO_CH              2U
#define BUZZER_PIN            4U    /* PTD4  */
#define BUZZER_ACTIVE_LOW     0
#define ENC_CLK_PIN          12U    /* PTA12 encoder CLK (D3) */
#define ENC_DT_PIN           13U    /* PTA13 encoder DT  (D8) */
#define ENC_REVERSE           false
#define SW3_PIN               4U    /* PTA4  onboard button, active-LOW */

#define SERVO_MOD          7499U
#define SERVO_MIN_CNV       225U    /* 0.6 ms */
#define SERVO_MAX_CNV       900U    /* 2.4 ms */
#define SERVO_US_PER_CNT_X100 267U  /* 2.67 us per count */

#define MAX_MSG_LEN          66U
#define TX_LEN               80U

/* ===================================================================
 * RTOS objects
 * =================================================================*/
typedef struct { char message[MAX_MSG_LEN]; } TMessage;

static QueueHandle_t     rxQueue;
static SemaphoreHandle_t printMutex;
static SemaphoreHandle_t txDoneSem;
static TaskHandle_t      encTaskHandle;

static char              txBuf[TX_LEN];
static volatile uint32_t txIdx;
static enc_t             encoder;         /* PORTA ISR writes, task reads in a critical section */
static volatile uint32_t encEdges;        /* raw pin-change interrupts seen */
static volatile uint32_t rxDropped;

#define LOG(...) do {                                                   \
        if (xSemaphoreTake(printMutex, pdMS_TO_TICKS(100)) == pdTRUE) { \
            PRINTF(__VA_ARGS__);                                        \
            xSemaphoreGive(printMutex);                                 \
        }                                                               \
    } while (0)

/* ===================================================================
 * Drivers
 * =================================================================*/
static void initLED(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTE_MASK;
    GPIOE->PSOR  = 1UL << RED_PIN;
    GPIOE->PDDR |= 1UL << RED_PIN;
    PORTE->PCR[RED_PIN] = PORT_PCR_MUX(1U);
}

static inline void setRedLED(bool on)
{
    if (on) GPIOE->PCOR = 1UL << RED_PIN;
    else    GPIOE->PSOR = 1UL << RED_PIN;
}

static void initBuzzer(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTD_MASK;
#if BUZZER_ACTIVE_LOW
    GPIOD->PSOR  = 1UL << BUZZER_PIN;
#else
    GPIOD->PCOR  = 1UL << BUZZER_PIN;
#endif
    GPIOD->PDDR |= 1UL << BUZZER_PIN;
    PORTD->PCR[BUZZER_PIN] = PORT_PCR_MUX(1U);
}

static inline void setBuzzer(bool on)
{
    if (on != (bool)BUZZER_ACTIVE_LOW) GPIOD->PSOR = 1UL << BUZZER_PIN;
    else                               GPIOD->PCOR = 1UL << BUZZER_PIN;
}

#if HAS_SERVO
static void initServo(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK;
    SIM->SCGC6 |= SIM_SCGC6_TPM0_MASK;
    PORTA->PCR[SERVO_PIN] = PORT_PCR_MUX(3U);

    SIM->SOPT2 &= ~SIM_SOPT2_TPMSRC_MASK;
    SIM->SOPT2 |=  SIM_SOPT2_TPMSRC(1U);

    TPM0->SC  = 0;
    TPM0->CNT = 0;
    TPM0->MOD = SERVO_MOD;
    TPM0->CONTROLS[SERVO_CH].CnSC = TPM_CnSC_MSB_MASK | TPM_CnSC_ELSB_MASK;
    TPM0->CONTROLS[SERVO_CH].CnV  = (SERVO_MIN_CNV + SERVO_MAX_CNV) / 2U;
    TPM0->SC  = TPM_SC_CMOD(1U) | TPM_SC_PS(7U);
}

/* Returns the CnV written, for printing. */
static uint32_t setServoDeg(uint32_t deg)
{
    if (deg > 180U) deg = 180U;
    uint32_t cnv = SERVO_MIN_CNV + deg * (SERVO_MAX_CNV - SERVO_MIN_CNV) / 180U;
    TPM0->CONTROLS[SERVO_CH].CnV = cnv;
    return cnv;
}
#endif /* HAS_SERVO */

static void initSW3(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK;
    PORTA->PCR[SW3_PIN] = PORT_PCR_MUX(1U) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK;  /* pull-up */
    GPIOA->PDDR &= ~(1UL << SW3_PIN);
}

static inline bool sw3Pressed(void)
{
    return (GPIOA->PDIR & (1UL << SW3_PIN)) == 0U;
}

static inline uint8_t encoderPins(void)
{
    uint32_t in = GPIOA->PDIR;
    return (uint8_t)((((in >> ENC_CLK_PIN) & 1U) << 1) | ((in >> ENC_DT_PIN) & 1U));
}

static void initEncoder(void)
{
    NVIC_DisableIRQ(PORTA_IRQn);
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK;

    const uint32_t pcr = PORT_PCR_MUX(1U) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK
                       | PORT_PCR_IRQC(0xBU);                    /* either edge */
    PORTA->PCR[ENC_CLK_PIN] = pcr | PORT_PCR_ISF_MASK;
    PORTA->PCR[ENC_DT_PIN]  = pcr | PORT_PCR_ISF_MASK;
    GPIOA->PDDR &= ~((1UL << ENC_CLK_PIN) | (1UL << ENC_DT_PIN));

    encoder_init(&encoder, encoderPins(), ENC_REVERSE);

    PORTA->ISFR = (1UL << ENC_CLK_PIN) | (1UL << ENC_DT_PIN);
    NVIC_SetPriority(PORTA_IRQn, PORTA_INT_PRIO);
    NVIC_ClearPendingIRQ(PORTA_IRQn);
    NVIC_EnableIRQ(PORTA_IRQn);
}

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
    (void)UART2->D;

    UART2->C2 = UART_C2_RE_MASK | UART_C2_RIE_MASK | UART_C2_TE_MASK;

    NVIC_SetPriority(UART2_FLEXIO_IRQn, UART2_INT_PRIO);
    NVIC_ClearPendingIRQ(UART2_FLEXIO_IRQn);
    NVIC_EnableIRQ(UART2_FLEXIO_IRQn);
}

/* ===================================================================
 * ISRs (same pattern as the main firmware)
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
            rxDropped++;
        }
        if (c == '\n') {
            if (!discard && recv_ptr != 0U) {
                msg.message[recv_ptr] = '\0';
                if (xQueueSendFromISR(rxQueue, &msg, &hpw) != pdTRUE) rxDropped++;
            }
            recv_ptr = 0;
            discard  = false;
        } else if (c != '\r' && !discard) {
            if (recv_ptr < MAX_MSG_LEN - 1U) {
                msg.message[recv_ptr++] = c;
            } else {
                recv_ptr = 0;
                discard  = true;
                rxDropped++;
            }
        }
    }

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
        PORTA->ISFR = mask;
        encEdges++;
        if (encoder_update(&encoder, encoderPins()))
            vTaskNotifyGiveFromISR(encTaskHandle, &hpw);
    }
    portYIELD_FROM_ISR(hpw);
}

/* ===================================================================
 * TASK: actuatorTestTask — LED, buzzer, servo sequence (boot + SW3)
 * =================================================================*/
static void runActuatorSequence(uint32_t run)
{
    LOG("\r\n===== Actuator test #%lu =====\r\n", (unsigned long)run);

    LOG("[LED]   ON for 1 s  -> check: red LED lit?\r\n");
    setRedLED(true);
    vTaskDelay(pdMS_TO_TICKS(1000));
    setRedLED(false);
    LOG("[LED]   OFF\r\n");
    vTaskDelay(pdMS_TO_TICKS(300));

    LOG("[BUZ]   3 beeps     -> check: three clicks/beeps heard?\r\n");
    for (int i = 0; i < 3; i++) {
        setBuzzer(true);
        vTaskDelay(pdMS_TO_TICKS(150));
        setBuzzer(false);
        vTaskDelay(pdMS_TO_TICKS(150));
    }

#if HAS_SERVO
    static const uint32_t angles[] = { 0U, 90U, 180U, 90U };
    for (uint32_t i = 0; i < sizeof angles / sizeof angles[0]; i++) {
        uint32_t cnv = setServoDeg(angles[i]);
        LOG("[SERVO] %3lu deg (CnV %lu, pulse ~%lu us)\r\n", (unsigned long)angles[i],
            (unsigned long)cnv, (unsigned long)(cnv * SERVO_US_PER_CNT_X100 / 100U));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    LOG("[SERVO] skipped (HAS_SERVO 0)\r\n");
#endif

    LOG("===== Actuator test done (press SW3 to repeat) =====\r\n\r\n");
}

static void actuatorTestTask(void *p)
{
    (void)p;
    uint32_t run = 1;
    bool wasPressed = false;

    vTaskDelay(pdMS_TO_TICKS(500));          /* let the banner print first */
    runActuatorSequence(run++);

    while (1) {
        bool pressed = sw3Pressed();
        if (pressed && !wasPressed) {
            vTaskDelay(pdMS_TO_TICKS(30));   /* debounce */
            if (sw3Pressed()) runActuatorSequence(run++);
        }
        wasPressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ===================================================================
 * TASK: encoderTestTask — throttle level per detent + 5 s summary
 * =================================================================*/
static void encoderTestTask(void *p)
{
    (void)p;
    int lastLevel = ENC_INITIAL;
    TickType_t lastSummary = xTaskGetTickCount();
    char bar[21];

    LOG("[ENC]   level %d%%, idle pins CLK=%u DT=%u (turn the knob)\r\n", lastLevel,
        (unsigned)(encoderPins() >> 1), (unsigned)(encoderPins() & 1U));

    while (1) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));

        taskENTER_CRITICAL();
        int level = encoder.level;
        uint32_t skipped = encoder.skipped;
        uint32_t edges = encEdges;
        taskEXIT_CRITICAL();

        if (level != lastLevel) {
            uint32_t fill = (uint32_t)level / 5U;
            for (uint32_t i = 0; i < 20U; i++) bar[i] = (i < fill) ? '#' : '.';
            bar[20] = '\0';
            LOG("[ENC]   %3d%% [%s] %s\r\n", level, bar, level > lastLevel ? "up" : "down");
            lastLevel = level;
        }

        if (xTaskGetTickCount() - lastSummary >= pdMS_TO_TICKS(5000)) {
            lastSummary = xTaskGetTickCount();
            LOG("[ENC]   level %d%%, %lu edges, %lu skipped%s\r\n", level,
                (unsigned long)edges, (unsigned long)skipped,
                edges == 0U ? "  (no edges yet: check CLK->D3, DT->D8, + -> 3V3, GND)" : "");
        }
    }
}

/* ===================================================================
 * TASK: linkTask — echo every UART line back as ECHO:<line>
 * =================================================================*/
static void linkTask(void *p)
{
    (void)p;
    TMessage line;
    uint32_t received = 0;
    TickType_t lastStats = xTaskGetTickCount();

    initUART2(BAUD_RATE);

    while (1) {
        if (xQueueReceive(rxQueue, &line, pdMS_TO_TICKS(1000)) == pdTRUE) {
            received++;
            LOG("[LINK]  rx: %s\r\n", line.message);

            strcpy(txBuf, "ECHO:");
            strncat(txBuf, line.message, TX_LEN - 7U);
            strcat(txBuf, "\n");
            txIdx = 0;
            UART2->C2 |= UART_C2_TIE_MASK;
            if (xSemaphoreTake(txDoneSem, pdMS_TO_TICKS(200)) != pdTRUE) {
                UART2->C2 &= ~UART_C2_TIE_MASK;
                LOG("[LINK]  TX timeout\r\n");
            }
        }

        if (xTaskGetTickCount() - lastStats >= pdMS_TO_TICKS(5000)) {
            lastStats = xTaskGetTickCount();
            LOG("[LINK]  %lu lines received, %lu dropped%s\r\n", (unsigned long)received,
                (unsigned long)rxDropped,
                received == 0U ? "  (nothing yet: check ESP32 TX -> PTE23, common GND)" : "");
        }
    }
}

/* ===================================================================
 * Hooks (required by the shared FreeRTOSConfig_Gen.h)
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

    initLED();
    initBuzzer();
#if HAS_SERVO
    initServo();
#endif
    initSW3();

    rxQueue    = xQueueCreate(5, sizeof(TMessage));
    printMutex = xSemaphoreCreateMutex();
    txDoneSem  = xSemaphoreCreateBinary();
    if (!rxQueue || !printMutex || !txDoneSem) {
        PRINTF("ERROR: RTOS object allocation failed\r\n");
        fatalError();
    }

    BaseType_t ok = pdPASS;
    ok &= xTaskCreate(linkTask,         "link",  configMINIMAL_STACK_SIZE + 200, NULL, 3, NULL);
    ok &= xTaskCreate(encoderTestTask,  "enc",   configMINIMAL_STACK_SIZE + 150, NULL, 2, &encTaskHandle);
    ok &= xTaskCreate(actuatorTestTask, "act",   configMINIMAL_STACK_SIZE + 150, NULL, 1, NULL);
    if (ok != pdPASS) {
        PRINTF("ERROR: task allocation failed\r\n");
        fatalError();
    }

    initEncoder();

    PRINTF("\r\n=== CG2271 MCXC444 hardware test ===\r\n");
    PRINTF("LED PTE31 | buzzer PTD4 (D10) | servo PTA5 (D5)%s | encoder CLK PTA12 (D3) DT PTA13 (D8)\r\n",
           HAS_SERVO ? "" : " disabled");
    PRINTF("UART2 %u baud: TX PTE22 -> ESP32 RX, RX PTE23 <- ESP32 TX, common GND\r\n", BAUD_RATE);

    vTaskStartScheduler();

    PRINTF("ERROR: scheduler could not start\r\n");
    fatalError();
    return 0;
}
