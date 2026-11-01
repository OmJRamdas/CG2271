#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "config.h"
#include "GoogleRoots.h"

// Only the worker task touches HTTP, tokens, pending jobs and completed results.
// The Arduino loop uses submit(), requestBest(), and receive() only. All queue
// messages contain values, never String objects or pointers into JSON documents.
namespace FirebaseWorker {
enum class ReplyType { Ready, Best, Score };
struct Reply {
  ReplyType type;
  char id[65];
  int score;
  int highScore;
  bool newRecord;
};
struct PendingScore {
  char runId[65]; int score; int highScore; bool newRecord; bool processed;
};
struct ReadRequest { char id[65]; };

static QueueHandle_t inbox = nullptr, reads = nullptr, outbox = nullptr;
static bool finished = false, attempted = false;
static uint32_t lastAttemptAt = 0, authenticatedAt = 0;
static String idToken;
static bool saveAttempted = false, readAttempted = false;
static uint32_t lastSaveAttemptAt = 0, lastReadAttemptAt = 0;
static PendingScore scoreQueue[8], completedScores[8];
static unsigned scoreHead = 0, scoreCount = 0;
static unsigned completedCount = 0, completedNext = 0;
static char pendingReadId[65] = "";
static int cachedHighScore = 0;

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

// These three functions are the queue-only interface used by the Arduino loop.
static bool submit(const char *runId, int score) {
  if (!inbox || !validId(runId) || score < 0 || score > 9999999) return false;
  PendingScore job = {};
  strcpy(job.runId, runId);
  job.score = score;
  return xQueueSend(inbox, &job, 0) == pdTRUE;
}

static bool requestBest(const char *id) {
  if (!reads || !validId(id)) return false;
  ReadRequest request = {};
  strcpy(request.id, id);
  // Coalesce repeated browser lookups without consuming score queue capacity.
  return xQueueOverwrite(reads, &request) == pdTRUE;
}

static bool receive(Reply &reply) {
  return outbox && xQueueReceive(outbox, &reply, 0) == pdTRUE;
}

static bool sendScoreResult(const PendingScore &job) {
  Reply reply = {};
  reply.type = ReplyType::Score;
  strcpy(reply.id, job.runId);
  reply.score = job.score;
  reply.highScore = job.highScore;
  reply.newRecord = job.newRecord;
  return xQueueSend(outbox, &reply, 0) == pdTRUE;
}

// Firebase HTTPS implementation follows. Called ONLY by taskMain().
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

static bool authenticate() {
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
  Reply ready = {};
  ready.type = ReplyType::Ready;
  xQueueSend(outbox, &ready, 0);
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
  Reply reply = {};
  reply.type = ReplyType::Best;
  strcpy(reply.id, pendingReadId);
  reply.highScore = cachedHighScore;
  if (xQueueSend(outbox, &reply, 0) == pdTRUE) pendingReadId[0] = '\0';
}

static void acceptCommands() {
  PendingScore command;
  // Bound the drain so a busy browser cannot starve Firebase work.
  for (unsigned n = 0; n < 8 && xQueueReceive(inbox, &command, 0) == pdTRUE; ++n) {
    bool duplicate = false;
    for (unsigned i = 0; i < completedCount; ++i) {
      if (!strcmp(completedScores[i].runId, command.runId)) {
        if (completedScores[i].score == command.score) sendScoreResult(completedScores[i]);
        duplicate = true;
        break;
      }
    }
    for (unsigned i = 0; i < scoreCount && !duplicate; ++i) {
      if (!strcmp(scoreQueue[(scoreHead + i) % 8].runId, command.runId)) duplicate = true;
    }
    if (duplicate) continue;
    if (scoreCount == 8) {
      Serial.println("[SCORE] Pending queue full; browser must retry submission.");
      continue;
    }
    scoreQueue[(scoreHead + scoreCount) % 8] = command;
    ++scoreCount;
    Serial.printf("[SCORE] Completed game %s: %d (queued for comparison)\n",
                  command.runId, command.score);
  }
  ReadRequest read;
  if (xQueueReceive(reads, &read, 0) == pdTRUE) strcpy(pendingReadId, read.id);
}

static void taskMain(void *) {
  bool waitingForTimeLogged = false;
  for (;;) {
    acceptCommands();
    const uint32_t now = millis();
    if (finished && uint32_t(now - authenticatedAt) >= 50UL * 60UL * 1000UL) {
      finished = false;
      attempted = false;
      idToken = "";
    }
    if (WiFi.status() == WL_CONNECTED) {
      if (time(nullptr) < 1767225600) {
        if (!waitingForTimeLogged) {
          Serial.println("[TIME] Waiting for network time; WebSocket handling continues.");
          waitingForTimeLogged = true;
        }
        // Don't start a retry timer here: authenticate as soon as time is ready.
      } else {
        if (!finished && (!attempted || uint32_t(now - lastAttemptAt) >= 10000)) {
          attempted = true;
          finished = authenticate();
          lastAttemptAt = millis();
          if (!finished) Serial.println("[AUTH] Will retry in 10 seconds.");
        }
        if (finished && scoreCount &&
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
        if (finished && pendingReadId[0] &&
            (!readAttempted || uint32_t(millis() - lastReadAttemptAt) >= 10000)) {
          replyToHighScoreRequest();
          lastReadAttemptAt = millis();
          readAttempted = true;
        }
      }
    }
    // Works on the single-core S2 too: no task is pinned to a particular core.
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

static bool begin() {
  inbox = xQueueCreate(8, sizeof(PendingScore));
  reads = xQueueCreate(1, sizeof(ReadRequest));
  outbox = xQueueCreate(12, sizeof(Reply));
  // ESP-IDF stack sizes are bytes. TLS/JSON require a larger task stack.
  if (inbox && reads && outbox &&
      xTaskCreate(taskMain, "firebase", 16384, nullptr, 1, nullptr) == pdPASS) return true;
  if (inbox) vQueueDelete(inbox);
  if (reads) vQueueDelete(reads);
  if (outbox) vQueueDelete(outbox);
  inbox = reads = outbox = nullptr;
  return false;
}
}  // namespace FirebaseWorker

