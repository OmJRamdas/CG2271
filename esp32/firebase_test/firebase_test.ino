/* Standalone high-score bridge for ESP32-S3/S2.
 * Arduino loop owns WebSocket and Wi-Fi lifecycle.
 * FirebaseWorker owns HTTPS and authentication; queues connect the two tasks.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "FirebaseWorker.h"

WebSocketsClient webSocket;
static bool wifiWasConnected = false;
static uint32_t lastWifiRetryAt = 0;
static FirebaseWorker::Reply pendingReply;
static bool hasPendingReply = false;

static void serviceReplies() {
  // Retain an unsent reply across a relay disconnect. Never block on the queue.
  if (!webSocket.isConnected()) return;
  if (!hasPendingReply) hasPendingReply = FirebaseWorker::receive(pendingReply);
  if (!hasPendingReply) return;
  JsonDocument result;
  if (pendingReply.type == FirebaseWorker::ReplyType::Ready) {
    result["type"] = "high_score_ready";
  } else {
    result["highScore"] = pendingReply.highScore;
    if (pendingReply.type == FirebaseWorker::ReplyType::Best) {
      result["type"] = "high_score";
      result["requestId"] = pendingReply.id;
    } else {
      result["type"] = "high_score_result";
      result["runId"] = pendingReply.id;
      result["score"] = pendingReply.score;
      result["newRecord"] = pendingReply.newRecord;
    }
  }
  String json;
  serializeJson(result, json);
  if (webSocket.sendTXT(json)) hasPendingReply = false;
}

void onWebSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[WS] Connected to Render");
      webSocket.sendTXT("{\"type\":\"identify\",\"role\":\"esp32\"}");
      // Notify browser to retry without reading any worker-owned state.
      webSocket.sendTXT("{\"type\":\"high_score_ready\"}");
      break;
    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected");
      break;
    case WStype_TEXT: {
      if (length > 512) break;
      JsonDocument message;
      if (deserializeJson(message, payload, length)) break;
      const char *kind = message["type"] | "";
      if (!strcmp(kind, "get_high_score")) {
        FirebaseWorker::requestBest(message["requestId"] | "");
      } else if (!strcmp(kind, "submit_score") && message["score"].is<int>()) {
        if (!FirebaseWorker::submit(message["runId"] | "", message["score"].as<int>())) {
          Serial.println("[SCORE] Invalid submission or request queue full; browser will retry.");
        }
      }
      // ROLL and legacy SCORE telemetry stay silent.
      break;
    }
    default: break;
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nFirebase high-score bridge (background worker)");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiRetryAt = millis();
  configTime(0, 0, "time.google.com", "pool.ntp.org");
  Serial.println("[WIFI] Connecting; WebSocket loop will keep running.");
  webSocket.onEvent(onWebSocketEvent);
  webSocket.beginSSL("skystrike-relay.onrender.com", 443, "/");
  webSocket.setReconnectInterval(5000);
  if (!FirebaseWorker::begin()) {
    Serial.println("[FATAL] Could not allocate Firebase queues/task. Reset the board.");
    while (true) delay(1000);
  }
}

void loop() {
  const uint32_t now = millis();
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != wifiWasConnected) {
    wifiWasConnected = connected;
    if (connected) {
      Serial.print("[WIFI] Connected, IP: ");
      Serial.println(WiFi.localIP());
      configTime(0, 0, "time.google.com", "pool.ntp.org");
    } else Serial.println("[WIFI] Disconnected; waiting to reconnect.");
  }
  if (!connected && uint32_t(now - lastWifiRetryAt) >= 30000) {
    lastWifiRetryAt = now;
    Serial.println("[WIFI] Retrying hotspot connection...");
    WiFi.reconnect();
  }
  if (connected) {
    webSocket.loop();
    serviceReplies();
  }
  delay(1);
}
