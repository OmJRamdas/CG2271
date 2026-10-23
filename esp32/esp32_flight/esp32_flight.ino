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
 * Rotary encoder (throttle) is on the MCXC444; it arrives here as $ENC.
 *
 * MCXC444 sends: $ENC / $ACK / $NAK (see mcx/source/protocol.h).
 * We send: $GS state heartbeats and $EV one-shot game events.
 *
 * UART wiring identical to Assignment 3:
 *   ESP32 GPIO1 TX  →  MCX PTE23 UART2 RX
 *   ESP32 GPIO2 RX  ←  MCX PTE22 UART2 TX
 *   GND             ↔  GND
 *
 * Required libraries (install via Arduino Library Manager):
 *   - WebSockets by Markus Sattler  (arduinoWebSockets)
 *   - Adafruit MPU6050, Adafruit Unified Sensor, Adafruit BusIO
 *   - ArduinoJson 7
 *   - (Wire.h is built-in)
 *
 * Board:  ESP32S2 Dev Module
 * Upload: Select correct serial port for ESP32-S2
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <esp_random.h>
#include "McxLink.h"
#include "ButtonEvents.h"
#include "TiltController.h"
#include "FireHold.h"

// Original accelerometer convention: +X -> +pitch, +Y -> +roll.
// The new estimator uses aerospace pitch (-X), so invert only its output.
static TiltConfig originalDirections() {
  TiltConfig config;
  config.pitchSign = -1.0f;
  return config;
}
TiltController tilt(originalDirections());

#if !defined(CONFIG_IDF_TARGET_ESP32S2)
#error "Select 'ESP32S2 Dev Module' in Arduino IDE."
#endif

/* ════════════════════════════════════════════
 *  PIN ASSIGNMENTS  (ESP32-S2-DevKitM-1)
 * ════════════════════════════════════════════ */
#define UART_TX_PIN   1    // Serial1 TX → MCX RX
#define UART_RX_PIN   2    // Serial1 RX ← MCX TX
#define USE_JOYSTICK  1    // encoder moved to the MCXC444, so GPIO5 is the joystick's again
#define JOY_X_PIN     3    // Joystick X axis (ADC)
#define JOY_Y_PIN     5    // Joystick Y axis (ADC)
#define BTN_PIN       7    // Fire button (active LOW, internal pull-up)
#define I2C_SDA_PIN   8    // MPU-6050 SDA
#define I2C_SCL_PIN   9    // MPU-6050 SCL

/* ════════════════════════════════════════════
 *  WIFI & WEBSOCKET CONFIG
 *  *** EDIT THESE BEFORE UPLOADING ***
 * ════════════════════════════════════════════ */
const char* WIFI_SSID = "S22";
const char* WIFI_PASS = "pzlc1245";
// Local relay: laptop's IPv4 on the hotspot (ipconfig), port 3000, WS_USE_SSL 0.
// Cloud relay: e.g. "cg2271-relay.onrender.com", port 443, WS_USE_SSL 1.
#define WS_USE_SSL      0
const char* WS_HOST   = "10.166.151.55";
const uint16_t WS_PORT = 3000;
const char* WS_PATH    = "/";

/* ════════════════════════════════════════════
 *  TUNING CONSTANTS
 * ════════════════════════════════════════════ */
#define SEND_INTERVAL_MS    50     // 20 Hz sensor updates to game
#define JOY_DEADZONE        80     // ADC units from center
#define CLOUD_STALE_MS      1500
#define ENC_STALE_MS        1500   // MCX sends $ENC at least every 500 ms

/* ════════════════════════════════════════════
 *  GLOBAL STATE
 * ════════════════════════════════════════════ */
WebSocketsClient webSocket;
bool wsConnected = false;

// Joystick calibration (read at startup)
int joyXCenter = 2048, joyYCenter = 2048;

// Count accepted falling edges atomically; never clear a press before sending.
ButtonEvents button;
portMUX_TYPE buttonMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t fireSession = 0, lastSentFireCount = 0;

// Debounced held-fire level. (Throttle comes from the MCX encoder via $ENC.)
FireHold fireHold;
int lastPrintedThrottle = -1;
bool lastPrintedHeld = false;
uint32_t lastPrintedPresses = 0;

McxLink mcx;
char uartBuf[McxLink::MAX_LINE + 1];
uint8_t uartPos = 0;
bool uartDiscard = false;
bool hasCloudState = false;
uint32_t cloudStateAt = 0;
unsigned long lastSendTime = 0;
TiltStatus lastTiltStatus = TiltStatus::NotStarted;

// Joystick camera controls retain their original axes, signs and deadzone.
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
  portENTER_CRITICAL_ISR(&buttonMux);
  button.edge(millis());
  portEXIT_CRITICAL_ISR(&buttonMux);
}

static uint32_t buttonCount() {
  portENTER_CRITICAL(&buttonMux);
  const uint32_t count = button.count();
  portEXIT_CRITICAL(&buttonMux);
  return count;
}

/* ════════════════════════════════════════════
 *  THROTTLE (rotary encoder on the MCXC444, received as $ENC)
 * ════════════════════════════════════════════ */
// 0..100 while the MCX reading is fresh; -1 otherwise, which the game treats
// as "no controller throttle" and falls back to Shift/Ctrl.
static int throttleLevel(uint32_t now) {
  if (!mcx.hasEnc || uint32_t(now - mcx.encAt) > ENC_STALE_MS) return -1;
  return static_cast<int>(mcx.encLevel);
}

/* ════════════════════════════════════════════
 *  UART TO/FROM MCXC444
 * ════════════════════════════════════════════ */

static void readUART() {
  while (Serial1.available()) {
    const char c = static_cast<char>(Serial1.read());
    if (c == '\n') {
      if (!uartDiscard && uartPos) {
        uartBuf[uartPos] = '\0';
        mcx.receive(uartBuf, millis());
      }
      uartPos = 0;
      uartDiscard = false;
    } else if (c != '\r' && !uartDiscard) {
      if (uartPos < sizeof(uartBuf) - 1) uartBuf[uartPos++] = c;
      else uartDiscard = true;  // Discard the entire oversized line.
    }
  }
}

static void serviceMCX(uint32_t now) {
  if (!wsConnected || !hasCloudState || uint32_t(now - cloudStateAt) > CLOUD_STALE_MS) {
    mcx.suspend();  // Let the MCX watchdog expose a lost cloud/browser link.
    return;
  }
  if (Serial1.availableForWrite() >= static_cast<int>(McxLink::MAX_LINE + 1)) {
    const char *frame = mcx.nextFrame(now);
    if (frame) Serial1.print(frame);
  }
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
      hasCloudState = false;
      mcx.suspend();
      Serial.println("[WS] Disconnected");
      break;

    case WStype_TEXT: {
      // Length-aware parsing: WebSocket payloads need not be null terminated.
      if (length > 512) break;
      JsonDocument doc;
      if (deserializeJson(doc, payload, length)) break;
      const char *kind = doc["type"] | "";
      if (!strcmp(kind, "game_state")) {
        if (!doc["lives"].is<unsigned>() || !doc["score"].is<unsigned long>() ||
            !doc["mode"].is<const char *>()) break;
        if (mcx.setState(doc["lives"].as<unsigned>(), doc["score"].as<unsigned long>(),
                         doc["mode"].as<const char *>())) {
          cloudStateAt = millis();
          hasCloudState = true;
        }
      } else if (!strcmp(kind, "event") && hasCloudState &&
                 uint32_t(millis() - cloudStateAt) <= CLOUD_STALE_MS) {
        const char *event = doc["event"] | "";
        if (!mcx.queueEvent(event)) Serial.printf("[MCX] Rejected/dropped event: %s\n", event);
      }
      break;
    }

    default:
      break;
  }
}

/* ════════════════════════════════════════════
 *  BUILD & SEND SENSOR JSON
 * ════════════════════════════════════════════ */
static bool sendSensorData(const TiltReading &reading, float jx, float jy, uint32_t presses,
                           int throttle, bool fireHeld) {
  if (!wsConnected) return false;
  char json[448];
  const int len = snprintf(json, sizeof(json),
    "{\"type\":\"sensor\",\"pitch\":%.3f,\"roll\":%.3f,\"yaw\":%.3f,"
    "\"tiltValid\":%s,\"joyX\":%.3f,\"joyY\":%.3f,\"fire\":%s,"
    "\"fireSession\":%lu,\"fireCount\":%lu,\"firePending\":%lu,"
    "\"fireHeld\":%s,\"throttle\":%d}",
    reading.pitch, reading.roll, reading.yaw, reading.valid ? "true" : "false", jx, jy,
    presses != lastSentFireCount ? "true" : "false",
    static_cast<unsigned long>(fireSession), static_cast<unsigned long>(presses),
    static_cast<unsigned long>(uint32_t(presses - lastSentFireCount)),
    fireHeld ? "true" : "false", throttle);
  return len > 0 && len < static_cast<int>(sizeof(json)) && webSocket.sendTXT(json, len);
}

// Recovery is explicit to avoid repeatedly blocking the loop on broken I2C.
// Serial: c = recalibrate, r = recenter, i = reinitialize a reconnected MPU.
static void serviceTilt() {
  tilt.update();
  if (Serial.available()) {
    switch (Serial.read()) {
      case 'c': tilt.startCalibration(); break;
      case 'r': tilt.recenter(); break;
      case 'i': tilt.begin(Wire, 0x68); break;
    }
  }
  const TiltStatus status = tilt.status();
  if (status != lastTiltStatus) {
    const char *names[] = {"not started", "calibrating: hold still, component side up",
                           "ready", "fault: check wiring, then send i", "stale"};
    Serial.printf("[TILT] %s\n", names[static_cast<unsigned>(status)]);
    lastTiltStatus = status;
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

  Serial.println("[ENC] throttle comes from the MCXC444 encoder ($ENC over UART)");

#if USE_JOYSTICK
  // Joystick calibration (read center position at rest)
  calibrateJoystick();
#endif

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

  // WebSocket: plain ws:// to a local relay, or wss:// to a cloud (HTTPS) relay
#if WS_USE_SSL
  webSocket.beginSSL(WS_HOST, WS_PORT, WS_PATH);
#else
  webSocket.begin(WS_HOST, WS_PORT, WS_PATH);
#endif
  webSocket.onEvent(onWebSocketEvent);
  webSocket.setReconnectInterval(5000);
  Serial.printf("[WS] Target: %s://%s:%u%s\n", WS_USE_SSL ? "wss" : "ws", WS_HOST,
                (unsigned)WS_PORT, WS_PATH);

  // Begin after blocking startup work; loop() collects calibration samples.
  fireSession = esp_random();
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);
  Wire.setTimeOut(20);
  tilt.begin(Wire, 0x68);
  Serial.println("[TILT] Keep still near level for about 2.25 seconds");
  Serial.println("[TILT] Serial commands: c=calibrate, r=recenter, i=reinitialize");
  Serial.println("\n[READY] Entering main loop\n");
}

/* ════════════════════════════════════════════
 *  MAIN LOOP
 * ════════════════════════════════════════════ */
void loop() {
  serviceTilt();  // Poll frequently; TiltController schedules samples at 100 Hz.
  webSocket.loop();
  readUART();
  const uint32_t now = millis();
  serviceMCX(now);
  fireHold.update(now, digitalRead(BTN_PIN) == LOW);

  if (uint32_t(now - lastSendTime) >= SEND_INTERVAL_MS) {
    lastSendTime = now;
    float jx = 0, jy = 0;
#if USE_JOYSTICK
    readJoystick(jx, jy);
#endif
    const uint32_t presses = buttonCount();
    const int throttle = throttleLevel(now);
    if (throttle != lastPrintedThrottle) {
      if (throttle < 0) Serial.println("[ENC] no fresh $ENC from MCX (keyboard throttle)");
      else              Serial.printf("[ENC] throttle %d%%\n", throttle);
      lastPrintedThrottle = throttle;
    }
    if (fireHold.held() != lastPrintedHeld || presses != lastPrintedPresses) {
      Serial.printf("[BTN] held=%d taps=%lu\n", fireHold.held(), static_cast<unsigned long>(presses));
      lastPrintedHeld = fireHold.held();
      lastPrintedPresses = presses;
    }
    if (sendSensorData(tilt.latest(), jx, jy, presses, throttle, fireHold.held()))
      lastSentFireCount = presses;
  }
  delay(1);
}
