/*
 * CG2271 A4: ESP32-S2 hardware test
 * ──────────────────────────────────
 * Checks every ESP32 sensor and the UART link to the MCXC444.
 * Open Serial Monitor at 115200 baud.
 *
 *   MPU-6050   I2C scan + WHO_AM_I, then pitch/roll from the accelerometer
 *   Joystick   raw X/Y (0..4095) and the resting centre
 *   Button     interrupt-driven press counter (debounced)
 *   UART link  sends PING,<n> every 1 s to the MCX, expects ECHO:PING,<n>
 *              (flash mcx/hwtest/source/cg2271_hwtest.c on the MCXC444)
 *
 * Board: ESP32S2 Dev Module. No extra libraries needed.
 */

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

/* ════════════════════════════════════════════
 *  PINS: same as esp32_flight.ino; change here if wired differently
 * ════════════════════════════════════════════ */
#define UART_TX_PIN   1    // Serial1 TX -> MCX PTE23 (UART2 RX)
#define UART_RX_PIN   2    // Serial1 RX <- MCX PTE22 (UART2 TX)
#define JOY_X_PIN     3    // Joystick VRX (ADC)
#define JOY_Y_PIN     5    // Joystick VRY (ADC)
#define BTN_PIN       7    // Push button to GND (internal pull-up)
#define I2C_SDA_PIN   8    // MPU-6050 SDA
#define I2C_SCL_PIN   9    // MPU-6050 SCL

#define UART_BAUD         9600
#define MPU_ADDR          0x68
#define REG_PWR_MGMT_1    0x6B
#define REG_WHO_AM_I      0x75
#define REG_ACCEL_XOUT    0x3B
#define ACCEL_SCALE       16384.0f   // +-2 g

#define STATUS_PERIOD_MS  200
#define PING_PERIOD_MS    1000
#define PING_TIMEOUT_MS   2000
#define BTN_DEBOUNCE_MS   50

/* ════════════════════════════════════════════
 *  STATE
 * ════════════════════════════════════════════ */
bool mpuOk = false;

volatile bool btnFlag = false;
unsigned long lastBtnMs = 0;
uint32_t btnPresses = 0;

uint32_t pingSeq = 0, pingSent = 0, pingOk = 0, pingLost = 0;
bool pingWaiting = false;
unsigned long pingSentMs = 0, lastPingMs = 0, lastRttMs = 0;
char uartBuf[80];
uint8_t uartPos = 0;

unsigned long lastStatusMs = 0;

/* ════════════════════════════════════════════
 *  MPU-6050
 * ════════════════════════════════════════════ */
static bool mpuWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool mpuRead(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)MPU_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static void i2cScan() {
  Serial.println("[I2C]  scanning...");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C]  device at 0x%02X%s\n", addr,
                    addr == 0x68 ? "  (MPU-6050, AD0 low)" : addr == 0x69 ? "  (MPU-6050, AD0 high: set MPU_ADDR 0x69)" : "");
      found++;
    }
  }
  if (!found) Serial.println("[I2C]  nothing found: check VCC 3.3 V, GND, SDA->GPIO8, SCL->GPIO9");
}

static void initMPU() {
  uint8_t who = 0;
  if (!mpuRead(REG_WHO_AM_I, &who, 1)) {
    Serial.println("[MPU]  FAIL: no response at 0x68");
    return;
  }
  // Genuine MPU-6050 returns 0x68; some clones return 0x70/0x72/0x98 but still work.
  Serial.printf("[MPU]  WHO_AM_I = 0x%02X %s\n", who, who == 0x68 ? "PASS" : "(not 0x68: clone chip? continuing)");
  mpuOk = mpuWrite(REG_PWR_MGMT_1, 0x00);   // wake up
  Serial.println(mpuOk ? "[MPU]  awake" : "[MPU]  FAIL: wake-up write failed");
  delay(100);
}

static bool readTilt(float &pitch, float &roll) {
  uint8_t b[6];
  if (!mpuOk || !mpuRead(REG_ACCEL_XOUT, b, 6)) return false;
  float ax = (int16_t)((b[0] << 8) | b[1]) / ACCEL_SCALE;
  float ay = (int16_t)((b[2] << 8) | b[3]) / ACCEL_SCALE;
  float az = (int16_t)((b[4] << 8) | b[5]) / ACCEL_SCALE;
  pitch = atan2f(ax, sqrtf(ay * ay + az * az)) * 180.0f / M_PI;
  roll  = atan2f(ay, sqrtf(ax * ax + az * az)) * 180.0f / M_PI;
  return true;
}

/* ════════════════════════════════════════════
 *  BUTTON (interrupt sets a flag; loop debounces)
 * ════════════════════════════════════════════ */
void IRAM_ATTR buttonISR() {
  btnFlag = true;
}

static void checkButton() {
  if (!btnFlag) return;
  btnFlag = false;
  unsigned long now = millis();
  if (now - lastBtnMs >= BTN_DEBOUNCE_MS) {
    lastBtnMs = now;
    btnPresses++;
    Serial.printf("[BTN]  press #%lu\n", (unsigned long)btnPresses);
  }
}

/* ════════════════════════════════════════════
 *  UART LINK (PING -> MCX -> ECHO:PING)
 * ════════════════════════════════════════════ */
static void handleLine(const char *line) {
  char expect[32];
  snprintf(expect, sizeof expect, "ECHO:PING,%lu", (unsigned long)pingSeq);
  if (pingWaiting && strcmp(line, expect) == 0) {
    pingWaiting = false;
    pingOk++;
    lastRttMs = millis() - pingSentMs;
  } else {
    Serial.printf("[LINK] unexpected: \"%s\"\n", line);
  }
}

static void serviceLink() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\n') {
      uartBuf[uartPos] = '\0';
      if (uartPos) handleLine(uartBuf);
      uartPos = 0;
    } else if (c != '\r') {
      if (uartPos < sizeof(uartBuf) - 1) uartBuf[uartPos++] = c;
      else uartPos = 0;
    }
  }

  unsigned long now = millis();
  if (pingWaiting && now - pingSentMs >= PING_TIMEOUT_MS) {
    pingWaiting = false;
    pingLost++;
    Serial.printf("[LINK] PING,%lu lost: check ESP TX(GPIO%d)->PTE23, ESP RX(GPIO%d)<-PTE22, common GND, MCX test running\n",
                  (unsigned long)pingSeq, UART_TX_PIN, UART_RX_PIN);
  }
  if (!pingWaiting && now - lastPingMs >= PING_PERIOD_MS) {
    lastPingMs = now;
    pingSeq++;
    pingSent++;
    pingWaiting = true;
    pingSentMs = now;
    Serial1.printf("PING,%lu\n", (unsigned long)pingSeq);
  }
}

/* ════════════════════════════════════════════
 *  SETUP / LOOP
 * ════════════════════════════════════════════ */
void setup() {
  Serial.begin(115200);
  delay(1500);   // give the USB serial monitor time to attach
  Serial.println("\n=== CG2271 ESP32-S2 hardware test ===");

  Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  Serial.printf("[LINK] Serial1 %d baud, TX GPIO%d, RX GPIO%d\n", UART_BAUD, UART_TX_PIN, UART_RX_PIN);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);
  i2cScan();
  initMPU();

  analogReadResolution(12);
  long sx = 0, sy = 0;
  for (int i = 0; i < 20; i++) { sx += analogRead(JOY_X_PIN); sy += analogRead(JOY_Y_PIN); delay(5); }
  Serial.printf("[JOY]  resting centre X=%ld Y=%ld (expect ~2000 each; 0 or 4095 = wiring fault)\n", sx / 20, sy / 20);

  pinMode(BTN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BTN_PIN), buttonISR, FALLING);
  Serial.printf("[BTN]  GPIO%d, interrupt on FALLING; press it to count\n", BTN_PIN);

  Serial.println("\nTilt the MPU, move the joystick through its full range, press the button.\n");
}

void loop() {
  checkButton();
  serviceLink();

  unsigned long now = millis();
  if (now - lastStatusMs >= STATUS_PERIOD_MS) {
    lastStatusMs = now;
    float pitch = 0, roll = 0;
    char mpu[32];
    if (readTilt(pitch, roll)) snprintf(mpu, sizeof mpu, "pitch=%+6.1f roll=%+6.1f", pitch, roll);
    else                       snprintf(mpu, sizeof mpu, "-- no data --");

    Serial.printf("MPU %s | JOY x=%4d y=%4d | BTN presses=%lu | LINK sent=%lu ok=%lu lost=%lu rtt=%lums\n",
                  mpu, analogRead(JOY_X_PIN), analogRead(JOY_Y_PIN), (unsigned long)btnPresses,
                  (unsigned long)pingSent, (unsigned long)pingOk, (unsigned long)pingLost,
                  (unsigned long)lastRttMs);
  }
  delay(2);
}
