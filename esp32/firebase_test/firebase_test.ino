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
#include <WebSocketsClient.h>

static bool finished = false;
static bool wifiWasConnected = false;
static bool attempted = false;
static uint32_t lastAttemptAt = 0;
static uint32_t lastWifiRetryAt = 0;
static String idToken;
static uint32_t authenticatedAt = 0;
static uint32_t lastSaveAttemptAt = 0;
static bool saveAttempted = false;
struct PendingScore {
  char runId[65]; int score; int highScore; bool newRecord; bool processed;
};
static PendingScore scoreQueue[8];
static unsigned scoreHead = 0, scoreCount = 0;
static PendingScore completedScores[8];
static unsigned completedCount = 0, completedNext = 0;
static char pendingReadId[65] = "";
static int cachedHighScore = 0;
static uint32_t lastReadAttemptAt = 0;
static bool readAttempted = false;

WebSocketsClient webSocket;

static bool sendScoreResult(const PendingScore &job) {
  if (!webSocket.isConnected()) return false;
  JsonDocument result;
  result["type"] = "high_score_result";
  result["runId"] = job.runId;
  result["score"] = job.score;
  result["highScore"] = job.highScore;
  result["newRecord"] = job.newRecord;
  String json;
  serializeJson(result, json);
  return webSocket.sendTXT(json);
}

static bool validId(const char *id) {
  const size_t length = strlen(id);
  if (!length || length > 64) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = id[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return true;
}

// Never print request URLs (database URLs contain an auth token), passwords,
// or authentication response bodies (which contain tokens).
static int request(const String &url, const char *method,
                   const String &body, String &response,
                   String *etag = nullptr, const char *ifMatch = nullptr) {
  WiFiClientSecure tls;
  tls.setCACert(GOOGLE_ROOTS);
  tls.setHandshakeTimeout(10);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(10000);
  if (!http.begin(tls, url)) return -1;
  const char *headers[] = {"ETag"};
  if (etag) {
    http.collectHeaders(headers, 1);
    http.addHeader("X-Firebase-ETag", "true");
  }
  if (ifMatch) http.addHeader("if-match", ifMatch);
  http.addHeader("Content-Type", "application/json");
  int code;
  if (!strcmp(method, "GET")) code = http.GET();
  else if (!strcmp(method, "PUT")) code = http.PUT(body);
  else code = http.POST(body);
  response = code > 0 ? http.getString() : "";
  if (etag) *etag = http.header("ETag");
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
  idToken = auth["idToken"].as<String>();
  authenticatedAt = millis();
  auth.clear();
  response = "";
  Serial.println("[AUTH] Signed in; device UID matches.");

  String base = DATABASE_URL;
  while (base.endsWith("/")) base.remove(base.length() - 1);
    const String highScoreUrl =
      base + "/skyStrike/highScore.json?auth=" + idToken;

  Serial.println("[FIREBASE] Reading high score...");
  code = request(highScoreUrl, "GET", "", response);

  if (code != 200) {
    Serial.printf("[FIREBASE] Read failed, HTTP/result %d\n", code);
    return false;
  }

  JsonDocument result;
  if (deserializeJson(result, response)) {
    Serial.println("[FIREBASE] Invalid response.");
    return false;
  }

  if (result.isNull()) {
    cachedHighScore = 0;
    Serial.println("[FIREBASE] No high score yet; starting at 0.");
  } else if (result.is<int>() && result.as<int>() >= 0) {
    cachedHighScore = result.as<int>();
    Serial.printf("[FIREBASE] Stored high score: %d\n",
                  result.as<int>());
  } else {
    Serial.println("[FIREBASE] Expected a non-negative number.");
    return false;
  }

  Serial.println("[READY] Waiting for a completed-game score.");
  if (webSocket.isConnected()) webSocket.sendTXT("{\"type\":\"high_score_ready\"}");
  return true;
}

// Run outside the WebSocket callback. Keep a failed job queued for retry.
static bool saveHighScore(PendingScore &job) {
  String base = DATABASE_URL;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  const String url = base + "/skyStrike/highScore.json?auth=" + idToken;
  String response, etag;
  int code = request(url, "GET", "", response, &etag);
  if (code != 200) {
    if (code == 401) { finished = false; attempted = false; idToken = ""; }
    Serial.printf("[FIREBASE] Read failed (%d); score remains queued.\n", code);
    return false;
  }
  JsonDocument value;
  if (deserializeJson(value, response) ||
      (!value.isNull() && (!value.is<int>() || value.as<int>() < 0))) {
    Serial.println("[FIREBASE] Invalid high score; score remains queued.");
    return false;
  }
  const int best = value.isNull() ? 0 : value.as<int>();
  if (job.score <= best) {
    cachedHighScore = job.highScore = best;
    job.newRecord = false;
    job.processed = true;
    Serial.printf("[SCORE] %d did not beat %d; database unchanged.\n", job.score, best);
    return true;
  }
  if (etag.isEmpty()) {
    Serial.println("[FIREBASE] Missing ETag; refusing an unprotected overwrite.");
    return false;
  }
  // Firebase rejects this PUT if another writer changed the record after GET.
  code = request(url, "PUT", String(job.score), response, nullptr, etag.c_str());
  if (code == 412) {
    Serial.println("[FIREBASE] Record changed; will reread before retrying.");
    return false;
  }
  if (code != 200) {
    if (code == 401) { finished = false; attempted = false; idToken = ""; }
    Serial.printf("[FIREBASE] Save failed (%d); score remains queued.\n", code);
    return false;
  }
  Serial.printf("[FIREBASE] New high score saved: %d (previous: %d).\n", job.score, best);
  cachedHighScore = job.highScore = job.score;
  job.newRecord = true;
  job.processed = true;
  return true;
}

static void replyToHighScoreRequest() {
  // Read again on browser connection, rather than serving an old local value.
  String base = DATABASE_URL;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  String response;
  const int code = request(base + "/skyStrike/highScore.json?auth=" + idToken,
                           "GET", "", response);
  JsonDocument value;
  if (code != 200 || deserializeJson(value, response) ||
      (!value.isNull() && (!value.is<int>() || value.as<int>() < 0))) {
    if (code == 401) { finished = false; attempted = false; idToken = ""; }
    Serial.printf("[FIREBASE] Best-score lookup failed (%d); will retry.\n", code);
    return;
  }
  cachedHighScore = value.isNull() ? 0 : value.as<int>();
  JsonDocument result;
  result["type"] = "high_score";
  result["requestId"] = pendingReadId;
  result["highScore"] = cachedHighScore;
  String json;
  serializeJson(result, json);
  if (webSocket.isConnected() && webSocket.sendTXT(json)) pendingReadId[0] = '\0';
}

void onWebSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[WS] Connected to Render");
      webSocket.sendTXT(
        "{\"type\":\"identify\",\"role\":\"esp32\"}"
      );
      if (finished) webSocket.sendTXT("{\"type\":\"high_score_ready\"}");
      break;

    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected");
      break;

    case WStype_TEXT: {
      if (length > 512) break;
      JsonDocument message;
      if (deserializeJson(message, payload, length)) break;

      const char* messageType = message["type"] | "";
      if (!strcmp(messageType, "get_high_score")) {
        const char *id = message["requestId"] | "";
        if (validId(id)) strcpy(pendingReadId, id);
        break;
      }
      // Ignore legacy SCORE telemetry, ROLL, and other gameplay events.
      if (strcmp(messageType, "submit_score") != 0) break;
      if (!message["runId"].is<const char*>() || !message["score"].is<int>()) break;
      const char* run = message["runId"].as<const char*>();
      const int score = message["score"].as<int>();
      if (!validId(run) || score < 0 || score > 9999999) break;
      // Replay the original outcome if the browser missed its response.
      for (unsigned i = 0; i < completedCount; ++i) {
        if (strcmp(completedScores[i].runId, run) == 0) {
          if (completedScores[i].score == score) sendScoreResult(completedScores[i]);
          return;
        }
      }
      for (unsigned i = 0; i < scoreCount; ++i) {
        if (strcmp(scoreQueue[(scoreHead + i) % 8].runId, run) == 0) return;
      }
      if (scoreCount == 8) {
        Serial.println("[SCORE] Queue full; submission rejected.");
        break;
      }
      PendingScore &job = scoreQueue[(scoreHead + scoreCount) % 8];
      strcpy(job.runId, run);
      job.score = score;
      job.processed = false;
      ++scoreCount;
      Serial.printf("[SCORE] Completed game %s: %d (queued for comparison)\n", run, score);
      break;
    }

    default:
      break;
  }
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
  webSocket.onEvent(onWebSocketEvent);
  webSocket.beginSSL("skystrike-relay.onrender.com", 443, "/");
  webSocket.setReconnectInterval(5000);
}

void loop() {
  webSocket.loop();
  const uint32_t now = millis();
  // Renew the login conservatively before Firebase's usual one-hour expiry.
  if (finished && uint32_t(now - authenticatedAt) >= 50UL * 60UL * 1000UL) {
    finished = false;
    attempted = false;
    idToken = "";
  }
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
  } else if (!finished && (!attempted || uint32_t(now - lastAttemptAt) >= 60000)) {
    attempted = true;
    lastAttemptAt = now;
    if (time(nullptr) < 1767225600) {
      Serial.println("[TIME] Waiting for network time; check hotspot internet access.");
    } else {
      finished = runTest();
      if (!finished) Serial.println("[TEST] Will retry in about 60 seconds.");
    }
  }
  if (connected && finished && scoreCount &&
      (!saveAttempted || uint32_t(millis() - lastSaveAttemptAt) >= 10000)) {
    saveAttempted = true;
    PendingScore &job = scoreQueue[scoreHead];
    const bool processed = job.processed || saveHighScore(job);
    lastSaveAttemptAt = millis();
    if (processed && sendScoreResult(job)) {
      completedScores[completedNext] = job;
      completedNext = (completedNext + 1) % 8;
      if (completedCount < 8) ++completedCount;
      scoreHead = (scoreHead + 1) % 8;
      --scoreCount;
      saveAttempted = false;
    }
  }
  if (connected && finished && pendingReadId[0] && webSocket.isConnected() &&
      (!readAttempted || uint32_t(millis() - lastReadAttemptAt) >= 10000)) {
    replyToHighScoreRequest();
    lastReadAttemptAt = millis();
    readAttempted = true;
  }
  delay(10);
}
