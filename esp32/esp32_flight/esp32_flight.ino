/*
 * CG2271 Assignment 4: ESP32-S2 Flight Controller
 * ─────────────────────────────────────────────────
 * Reads sensors → WiFi → WebSocket relay → browser game.
 * Game events flow back over the same path and are forwarded
 * to the MCXC444 via UART for actuator control.
 *
 * Sensors on this board:
 *   MPU-6050 (I2C)  → pitch, roll, yaw   (flight control)
 *   Joystick (ADC)  → camera X / Y       (camera orbit)
 *   Push button     → fire               (GPIO interrupt)
 *
 * MCXC444 sends:  SENSOR:POT,<0..4095>\n   (potentiometer)
 * We forward:     GAME:<event>,<value>\n    (game events)
 *
 * UART wiring identical to Assignment 3:
 *   ESP32 GPIO1 TX  →  MCX PTE23 UART2 RX
 *   ESP32 GPIO2 RX  ←  MCX PTE22 UART2 TX
 *   GND             ↔  GND
 *
 * Required libraries (install via Arduino Library Manager):
 *   - WebSockets by Markus Sattler  (arduinoWebSockets)
 *   - (Wire.h is built-in)
 *
 * Board:  ESP32S2 Dev Module
 * Upload: Select correct serial port for ESP32-S2
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <Wire.h>
#include <math.h>

#if !defined(CONFIG_IDF_TARGET_ESP32S2)
#error "Select 'ESP32S2 Dev Module' in Arduino IDE."
#endif

/* ════════════════════════════════════════════
 *  PIN ASSIGNMENTS  (ESP32-S2-DevKitM-1)
 * ════════════════════════════════════════════ */
#define UART_TX_PIN   1    // Serial1 TX → MCX RX
#define UART_RX_PIN   2    // Serial1 RX ← MCX TX
#define JOY_X_PIN     3    // Joystick X axis (ADC)
#define JOY_Y_PIN     5    // Joystick Y axis (ADC)
#define BTN_PIN       7    // Fire button (active LOW, internal pull-up)
#define I2C_SDA_PIN   8    // MPU-6050 SDA
#define I2C_SCL_PIN   9    // MPU-6050 SCL

/* ════════════════════════════════════════════
 *  WIFI & WEBSOCKET CONFIG
 *  *** EDIT THESE BEFORE UPLOADING ***
 * ════════════════════════════════════════════ */
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* WS_HOST   = "your-relay.glitch.me";  // e.g. "cg2271-relay.glitch.me"
const uint16_t WS_PORT = 443;
const char* WS_PATH    = "/";

/* ════════════════════════════════════════════
 *  MPU-6050 REGISTERS
 * ════════════════════════════════════════════ */
#define MPU_ADDR       0x68
#define REG_PWR_MGMT_1 0x6B
#define REG_ACCEL_CFG  0x1C
#define REG_GYRO_CFG   0x1B
#define REG_ACCEL_XOUT 0x3B   // 6 bytes: AX_H, AX_L, AY_H, AY_L, AZ_H, AZ_L
#define REG_GYRO_XOUT  0x43   // 6 bytes: GX_H, GX_L, GY_H, GY_L, GZ_H, GZ_L

// Sensitivity at default ±2g / ±250 deg/s
#define ACCEL_SCALE  16384.0f
#define GYRO_SCALE   131.0f

/* ════════════════════════════════════════════
 *  TUNING CONSTANTS
 * ════════════════════════════════════════════ */
#define SEND_INTERVAL_MS    50     // 20 Hz sensor updates to game
#define CALIB_SAMPLES       200    // gyro calibration at startup
#define COMP_ALPHA          0.96f  // complementary filter weight (gyro trust)
#define PITCH_MAX_DEG       45.0f  // tilt range mapped to ±1
#define ROLL_MAX_DEG        45.0f
#define YAW_RATE_MAX_DPS    90.0f  // gyro-Z rate mapped to ±1
#define JOY_DEADZONE        80     // ADC units from center
#define BTN_DEBOUNCE_MS     50

/* ════════════════════════════════════════════
 *  GLOBAL STATE
 * ════════════════════════════════════════════ */
WebSocketsClient webSocket;
bool wsConnected = false;

// MPU-6050 calibration
float gxOffset = 0.0f, gyOffset = 0.0f, gzOffset = 0.0f;

// Complementary filter state
float filtPitch = 0.0f, filtRoll = 0.0f;
unsigned long lastMPUMicros = 0;

// Joystick calibration (read at startup)
int joyXCenter = 2048, joyYCenter = 2048;

// Button (interrupt-driven)
volatile bool btnFlag = false;
unsigned long lastBtnTime = 0;

// Potentiometer from MCX via UART
float potValue = -1.0f;   // -1 = not yet received

// UART receive buffer
char uartBuf[64];
uint8_t uartPos = 0;

// Sensor send timing
unsigned long lastSendTime = 0;

/* ════════════════════════════════════════════
 *  MPU-6050 I2C HELPERS
 * ════════════════════════════════════════════ */
static void mpuWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

static void mpuRead(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)MPU_ADDR, len);
  for (uint8_t i = 0; i < len && Wire.available(); i++) {
    buf[i] = Wire.read();
  }
}

static int16_t combine(uint8_t h, uint8_t l) {
  return (int16_t)((h << 8) | l);
}

/* ════════════════════════════════════════════
 *  MPU-6050 INIT & CALIBRATION
 * ════════════════════════════════════════════ */
static void initMPU() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);  // 400 kHz fast-mode
  delay(100);

  // Wake up (clear sleep bit)
  mpuWrite(REG_PWR_MGMT_1, 0x00);
  delay(100);

  // ±2g accel, ±250 deg/s gyro (defaults, explicit for clarity)
  mpuWrite(REG_ACCEL_CFG, 0x00);
  mpuWrite(REG_GYRO_CFG,  0x00);
  delay(50);

  Serial.println("[MPU] Calibrating gyroscope — keep the board still…");
  float gxSum = 0, gySum = 0, gzSum = 0;
  for (int i = 0; i < CALIB_SAMPLES; i++) {
    uint8_t buf[6];
    mpuRead(REG_GYRO_XOUT, buf, 6);
    gxSum += combine(buf[0], buf[1]) / GYRO_SCALE;
    gySum += combine(buf[2], buf[3]) / GYRO_SCALE;
    gzSum += combine(buf[4], buf[5]) / GYRO_SCALE;
    delay(5);
  }
  gxOffset = gxSum / CALIB_SAMPLES;
  gyOffset = gySum / CALIB_SAMPLES;
  gzOffset = gzSum / CALIB_SAMPLES;
  Serial.printf("[MPU] Gyro offsets: %.2f, %.2f, %.2f deg/s\n", gxOffset, gyOffset, gzOffset);
}

/* ════════════════════════════════════════════
 *  READ MPU-6050 & COMPLEMENTARY FILTER
 * ════════════════════════════════════════════ */
struct SensorData {
  float pitch;   // -1..1
  float roll;    // -1..1
  float yaw;     // -1..1 (rate, not angle)
};

static SensorData readMPU() {
  SensorData out = {0, 0, 0};

  // Read accel + gyro (14 bytes: accel[6] + temp[2] + gyro[6])
  uint8_t buf[14];
  mpuRead(REG_ACCEL_XOUT, buf, 14);

  float ax = combine(buf[0],  buf[1])  / ACCEL_SCALE;
  float ay = combine(buf[2],  buf[3])  / ACCEL_SCALE;
  float az = combine(buf[4],  buf[5])  / ACCEL_SCALE;
  // buf[6..7] = temperature (skip)
  float gx = combine(buf[8],  buf[9])  / GYRO_SCALE - gxOffset;
  float gy = combine(buf[10], buf[11]) / GYRO_SCALE - gyOffset;
  float gz = combine(buf[12], buf[13]) / GYRO_SCALE - gzOffset;

  // Time delta
  unsigned long now = micros();
  float dt = (now - lastMPUMicros) / 1000000.0f;
  if (dt <= 0 || dt > 0.5f) dt = 0.02f;   // sanity clamp
  lastMPUMicros = now;

  // Accelerometer angles (degrees)
  float accPitch = atan2f(ax, sqrtf(ay * ay + az * az)) * 180.0f / M_PI;
  float accRoll  = atan2f(ay, sqrtf(ax * ax + az * az)) * 180.0f / M_PI;

  // Complementary filter: trust gyro short-term, accel long-term
  filtPitch = COMP_ALPHA * (filtPitch + gx * dt) + (1.0f - COMP_ALPHA) * accPitch;
  filtRoll  = COMP_ALPHA * (filtRoll  + gy * dt) + (1.0f - COMP_ALPHA) * accRoll;

  // Map to -1..1 for game
  out.pitch = constrain(filtPitch / PITCH_MAX_DEG, -1.0f, 1.0f);
  out.roll  = constrain(filtRoll  / ROLL_MAX_DEG,  -1.0f, 1.0f);
  // Yaw: use gyro Z rate directly (rotate controller to yaw)
  out.yaw   = constrain(gz / YAW_RATE_MAX_DPS, -1.0f, 1.0f);

  return out;
}

/* ════════════════════════════════════════════
 *  JOYSTICK
 * ════════════════════════════════════════════ */
static void calibrateJoystick() {
  long sumX = 0, sumY = 0;
  for (int i = 0; i < 30; i++) {
    sumX += analogRead(JOY_X_PIN);
    sumY += analogRead(JOY_Y_PIN);
    delay(5);
  }
  joyXCenter = sumX / 30;
  joyYCenter = sumY / 30;
  Serial.printf("[JOY] Center: X=%d, Y=%d\n", joyXCenter, joyYCenter);
}

static void readJoystick(float &jx, float &jy) {
  int rawX = analogRead(JOY_X_PIN) - joyXCenter;
  int rawY = analogRead(JOY_Y_PIN) - joyYCenter;

  // Deadzone
  if (abs(rawX) < JOY_DEADZONE) rawX = 0;
  if (abs(rawY) < JOY_DEADZONE) rawY = 0;

  // Map to -1..1 (assuming ≈2048 range from center to edge)
  jx = constrain((float)rawX / 1800.0f, -1.0f, 1.0f);
  jy = constrain((float)rawY / 1800.0f, -1.0f, 1.0f);
}

/* ════════════════════════════════════════════
 *  BUTTON (interrupt)
 * ════════════════════════════════════════════ */
void IRAM_ATTR buttonISR() {
  btnFlag = true;
}

static bool readButton() {
  if (btnFlag) {
    unsigned long now = millis();
    if (now - lastBtnTime >= BTN_DEBOUNCE_MS) {
      lastBtnTime = now;
      btnFlag = false;
      return true;
    }
    btnFlag = false;
  }
  return false;
}

/* ════════════════════════════════════════════
 *  UART TO/FROM MCXC444
 * ════════════════════════════════════════════ */

// Parse incoming: SENSOR:POT,<value>
static void processUARTLine(const char *line) {
  // Expected: "SENSOR:POT,<0..4095>"
  if (strncmp(line, "SENSOR:POT,", 11) == 0) {
    int raw = atoi(line + 11);
    potValue = constrain((float)raw / 4095.0f, 0.0f, 1.0f);
  }
}

static void readUART() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\n') {
      if (uartPos > 0) {
        uartBuf[uartPos] = '\0';
        processUARTLine(uartBuf);
        uartPos = 0;
      }
    } else if (c != '\r') {
      if (uartPos < sizeof(uartBuf) - 1) {
        uartBuf[uartPos++] = c;
      } else {
        uartPos = 0;   // overflow → discard
      }
    }
  }
}

// Forward game events to MCX: GAME:<event>,<value>\n
static void forwardToMCX(const char *event, int value) {
  Serial1.printf("GAME:%s,%d\n", event, value);
}

/* ════════════════════════════════════════════
 *  WEBSOCKET
 * ════════════════════════════════════════════ */
static void onWebSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {

    case WStype_CONNECTED:
      wsConnected = true;
      Serial.println("[WS] Connected to relay");
      webSocket.sendTXT("{\"type\":\"identify\",\"role\":\"esp32\"}");
      break;

    case WStype_DISCONNECTED:
      wsConnected = false;
      Serial.println("[WS] Disconnected");
      break;

    case WStype_TEXT: {
      // Parse game events: {"type":"event","event":"HIT","value":1}
      // Simple string-based parser (no ArduinoJson dependency)
      char *s = (char *)payload;

      // Find "event":"<name>"
      char *evPtr = strstr(s, "\"event\":\"");
      if (!evPtr) break;
      evPtr += 9;  // skip past "event":"
      char *evEnd = strchr(evPtr, '"');
      if (!evEnd) break;
      *evEnd = '\0';
      const char *eventName = evPtr;

      // Find "value":
      char *valPtr = strstr(evEnd + 1, "\"value\":");
      int value = 0;
      if (valPtr) {
        valPtr += 8;
        value = atoi(valPtr);
      }

      Serial.printf("[WS] Event: %s = %d\n", eventName, value);
      forwardToMCX(eventName, value);
      break;
    }

    default:
      break;
  }
}

/* ════════════════════════════════════════════
 *  BUILD & SEND SENSOR JSON
 * ════════════════════════════════════════════ */
static void sendSensorData(SensorData &mpu, float jx, float jy, bool fire) {
  if (!wsConnected) return;

  // Build JSON with sprintf (avoids ArduinoJson dependency)
  char json[256];
  int len;

  if (potValue >= 0) {
    len = snprintf(json, sizeof(json),
      "{\"type\":\"sensor\","
      "\"pitch\":%.3f,\"roll\":%.3f,\"yaw\":%.3f,"
      "\"joyX\":%.3f,\"joyY\":%.3f,"
      "\"fire\":%s,"
      "\"pot\":%.3f}",
      mpu.pitch, mpu.roll, mpu.yaw,
      jx, jy,
      fire ? "true" : "false",
      potValue
    );
  } else {
    len = snprintf(json, sizeof(json),
      "{\"type\":\"sensor\","
      "\"pitch\":%.3f,\"roll\":%.3f,\"yaw\":%.3f,"
      "\"joyX\":%.3f,\"joyY\":%.3f,"
      "\"fire\":%s}",
      mpu.pitch, mpu.roll, mpu.yaw,
      jx, jy,
      fire ? "true" : "false"
    );
  }

  if (len > 0 && len < (int)sizeof(json)) {
    webSocket.sendTXT(json);
  }
}

/* ════════════════════════════════════════════
 *  SETUP
 * ════════════════════════════════════════════ */
void setup() {
  // Debug serial
  Serial.begin(115200);
  delay(500);
  Serial.println("\n═══ CG2271 Sky Strike — ESP32-S2 Flight Controller ═══\n");

  // UART to MCXC444 (same pins as Assignment 3)
  Serial1.begin(9600, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  Serial.printf("[UART] TX=GPIO%d, RX=GPIO%d, 9600 baud\n", UART_TX_PIN, UART_RX_PIN);

  // ADC resolution (12-bit: 0–4095)
  analogReadResolution(12);

  // Button with internal pull-up + interrupt
  pinMode(BTN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BTN_PIN), buttonISR, FALLING);
  Serial.printf("[BTN] GPIO%d, interrupt on FALLING\n", BTN_PIN);

  // MPU-6050
  initMPU();
  lastMPUMicros = micros();

  // Joystick calibration (read center position at rest)
  calibrateJoystick();

  // WiFi
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - wifiStart > 15000) {
      Serial.println("\n[WiFi] TIMEOUT — continuing without WiFi");
      break;
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected!  IP: %s\n", WiFi.localIP().toString().c_str());
  }

  // WebSocket (SSL for Glitch/HTTPS)
  webSocket.beginSSL(WS_HOST, WS_PORT, WS_PATH);
  webSocket.onEvent(onWebSocketEvent);
  webSocket.setReconnectInterval(5000);
  Serial.printf("[WS] Target: wss://%s%s\n", WS_HOST, WS_PATH);

  Serial.println("\n[READY] Entering main loop\n");
}

/* ════════════════════════════════════════════
 *  MAIN LOOP
 * ════════════════════════════════════════════ */
void loop() {
  // Service WebSocket (must call frequently)
  webSocket.loop();

  // Read UART from MCX (pot sensor data)
  readUART();

  // Read all sensors
  SensorData mpu = readMPU();
  float jx, jy;
  readJoystick(jx, jy);
  bool fire = readButton();

  // Send sensor data to game at fixed interval
  unsigned long now = millis();
  if (now - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = now;
    sendSensorData(mpu, jx, jy, fire);
  }

  // Small yield to avoid WDT
  delay(1);
}
