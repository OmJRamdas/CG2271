/* Standalone Firebase smoke test; upload instead of the flight sketch.
 * Requires ArduinoJson 7 and the Arduino ESP32 board package.
 * HTTPS requests block for bounded periods: do NOT copy this loop directly
 * into the flight controller, which needs continuous sensor sampling.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "config.h"
#include "GoogleRoots.h"

static bool finished = false;
static bool wifiWasConnected = false;
static bool attempted = false;
static uint32_t lastAttemptAt = 0;
static uint32_t lastWifiRetryAt = 0;

// Never print request URLs (database URLs contain an auth token), passwords,
// or authentication response bodies (which contain tokens).
static int request(const String &url, const char *method,
                   const String &body, String &response) {
  WiFiClientSecure tls;
  tls.setCACert(GOOGLE_ROOTS);
  tls.setHandshakeTimeout(10);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(10000);
  if (!http.begin(tls, url)) return -1;
  http.addHeader("Content-Type", "application/json");
  int code;
  if (!strcmp(method, "GET")) code = http.GET();
  else if (!strcmp(method, "PUT")) code = http.PUT(body);
  else code = http.POST(body);
  response = code > 0 ? http.getString() : "";
  http.end();
  return code;
}

static bool runTest() {
  Serial.println("[AUTH] Signing in...");
  JsonDocument credentials;
  credentials["email"] = FIREBASE_EMAIL;
  credentials["password"] = FIREBASE_PASSWORD;
  credentials["returnSecureToken"] = true;
  String body, response;
  serializeJson(credentials, body);
  const String authUrl =
      String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") +
      FIREBASE_API_KEY;
  int code = request(authUrl, "POST", body, response);
  body = "";
  credentials.clear();
  if (code != 200) {
    Serial.printf("[AUTH] Failed, HTTP/result %d. Check email/password, API key,\n", code);
    Serial.println("       Email/Password provider, internet, and TLS/time settings.");
    return false;
  }
  JsonDocument auth;
  if (deserializeJson(auth, response) || !auth["idToken"].is<const char *>() ||
      !auth["localId"].is<const char *>()) {
    Serial.println("[AUTH] Invalid response.");
    return false;
  }
  if (strcmp(auth["localId"].as<const char *>(), DEVICE_UID)) {
    Serial.println("[AUTH] UID mismatch: this login is not the intended device account.");
    return false;
  }
  const String token = auth["idToken"].as<String>();
  auth.clear();
  response = "";
  Serial.println("[AUTH] Signed in; device UID matches.");

  String base = DATABASE_URL;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  const String testUrl = base + "/skyStrike/test.json?auth=" + token;
  Serial.println("[WRITE] Setting /skyStrike/test to 123...");
  code = request(testUrl, "PUT", "123", response);
  if (code != 200) {
    Serial.printf("[WRITE] Failed, HTTP/result %d. Check database URL and published rules.\n", code);
    return false;
  }
  code = request(testUrl, "GET", "", response);
  if (code != 200) {
    Serial.printf("[READ] Failed, HTTP/result %d.\n", code);
    return false;
  }
  JsonDocument value;
  if (deserializeJson(value, response) || !value.is<int>() || value.as<int>() != 123) {
    Serial.println("[READ] Value did not match 123.");
    return false;
  }
  Serial.println("[PASS] Wrote 123 and read back 123 from Firebase.");
  Serial.println("Check Realtime Database > Data > skyStrike > test.");
  Serial.println("Test finished; no more requests until you reset the board.");
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nFirebase standalone test");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiRetryAt = millis();
  // Certificate validation requires a correct clock. Synchronization is async.
  configTime(0, 0, "time.google.com", "pool.ntp.org");
  Serial.println("[WIFI] Connecting; waiting for internet and time synchronization...");
}

void loop() {
  if (finished) { delay(20); return; }
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
  if (!connected) {
    if (uint32_t(now - lastWifiRetryAt) >= 30000) {
      lastWifiRetryAt = now;
      Serial.println("[WIFI] Retrying hotspot connection...");
      WiFi.reconnect();
    }
  } else if (!attempted || uint32_t(now - lastAttemptAt) >= 60000) {
    attempted = true;
    lastAttemptAt = now;
    if (time(nullptr) < 1767225600) {
      Serial.println("[TIME] Waiting for network time; check hotspot internet access.");
    } else {
      finished = runTest();
      if (!finished) Serial.println("[TEST] Will retry in about 60 seconds.");
    }
  }
  delay(10);
}
