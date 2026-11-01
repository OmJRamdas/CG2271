# Firebase high-score test

## Current behavior

The sketch now reads `/skyStrike/highScore` instead of writing `test: 123`.
It connects to `wss://skystrike-relay.onrender.com` as the ESP32 client and accepts
`{"type":"submit_score","runId":"unique-id","score":30}` from the updated game.
Legacy SCORE/ROLL events are ignored. Only connect one ESP32 to this relay.

Completed scores enter an eight-entry RAM queue, including before authentication
finishes. The sketch rereads Firebase before each comparison. Lower/equal scores
do not write; higher scores use an ETag-conditional PUT so a concurrent change
cannot be overwritten without another read and comparison. Failed operations
stay queued and retry after 10 seconds. Queue overflow is reported as a rejection.
Queued scores are lost on reset. Queued runs and the last eight completed results
are deduplicated in memory; there is no persistent game-history deduplication yet.
Completed results include the run ID, submitted score, high score, and new-record
flag, and can be replayed to the browser after a lost response. An unacknowledged
PUT followed by a reset/network failure can still lose the new-record flag even
though the database value was saved; this is not a durable event delivery system.

Authentication is reused and renewed by signing in again after 50 minutes or an
HTTP 401 response. Browser replies and the best-score display are implemented;
MCU celebration events are not. HTTPS is still synchronous in this standalone
test; do not merge into the sensor loop until networking is separated.

Upload to your personal **ESP32S3 Dev Module** (S2 is also supported). Install
**ArduinoJson 7** and **WebSockets by Markus Sattler**. Keep the existing private
`config.h` and published device UID rules. Open the updated local `game/index.html`
and connect it to the relay; the hosted game needs a redeploy to send submit_score.
The connection address is prefilled. Connect on the start screen to load the best
score. On game over, the UI waits for an ESP32 result before announcing a record.
It retries pending requests every 10 seconds, including when the ESP32 starts
later than the browser. Unconfirmed scores remain only in browser memory, so
closing/reloading the page before saving can lose them.

WebSocket messages:
- Browser: `{"type":"get_high_score","requestId":"unique-id"}`
- ESP32: `{"type":"high_score","requestId":"unique-id","highScore":30}`
- Browser: `{"type":"submit_score","runId":"unique-run","score":40}`
- ESP32: `{"type":"high_score_result","runId":"unique-run","score":40,"highScore":40,"newRecord":true}`
- ESP32 announces `{"type":"high_score_ready"}` on Firebase startup/relay reconnect
  so the browser can request the record again.

Hardware verification:
1. Finish a game with a score above Firebase's current highScore. Expect
   `[FIREBASE] New high score saved: ...` and verify the Data tab.
2. Finish with a lower or equal score. Expect `database unchanged`.
3. Reset the ESP32. Confirm startup reads the saved record.
4. Submit before startup authentication completes. Confirm it is processed later.
5. Lose internet after receiving a score. Restore it and verify the queued retry.
6. Connect before/after ESP32 startup. Check the same best score appears on the
   start screen, HUD and game-over screen. Only a saved higher score should show
   `New record!`; a lower/equal score should show `best score unchanged`.

The remaining sections describe the original smoke test for reference; its
`test: 123` messages no longer apply to the current sketch.

## Original smoke test

This standalone sketch signs in as your device account, checks its UID, writes
the number `123` to `/skyStrike/test`, then reads and verifies it. It does not
change the flight game source or require an MPU, joystick, relay, or MCX.
Uploading it replaces the flight firmware on the ESP32 until you upload that again.

## Setup

1. Firebase Authentication: enable Email/Password and create the device user.
2. Realtime Database > Rules: paste `database.rules.json` and click Publish.
   These initial rules allow only the specified device UID under `/skyStrike`.
   Do not enable App Check enforcement for this basic REST test.
3. Copy `config.example.h` to `config.h` in this folder. Fill in hotspot name,
   hotspot password, Web API key, device account email, and device account password.
   URL and UID are already filled in. Do not append the console's `: null` display
   to the URL. `null` simply means the database currently has no data.
4. Open `firebase_test.ino` in Arduino IDE. Select ESP32S2 Dev Module and your port.
5. Install **ArduinoJson by Benoit Blanchon, version 7** using Library Manager.
   WiFi, WiFiClientSecure and HTTPClient come with the ESP32 board package.
   This sketch does not need a Firebase library or the browser JavaScript SDK.
6. Upload and open Serial Monitor at **115200 baud**. Enable mobile data on the
   hotspot phone; local Wi-Fi alone cannot reach Firebase.

Expected final line: `[PASS] Wrote 123 and read back 123 from Firebase.`

In the Firebase console's Data tab, expand `skyStrike` to see `test: 123`.
The sketch stops database requests after success. Reset to run again. It overwrites
only `/skyStrike/test`, so keep real scores and game data at other paths.

## Troubleshooting

- No Wi-Fi IP: check hotspot credentials and 2.4 GHz compatibility.
- Waiting for time: internet or network time access is unavailable. HTTPS needs a
  correct clock to validate server certificates.
- Authentication 400: check API key, device login, and Email/Password provider.
- UID mismatch: the email/password signed into a different device account.
- Database 401/403: check published UID rules, token access, and App Check settings.
- Negative HTTP result: network/TLS connection failure; check internet and clock.
- Database 404: check the database URL.

`config.h` is ignored by Git, but manually made source ZIPs can still include it.
Exclude it before sharing. Authentication tokens and passwords are never printed.

## Scope and next step

This is a bounded, synchronous HTTPS smoke test, not a production Firebase client.
It signs in again on each failed test attempt rather than retaining expired tokens.
The integrated version must renew tokens and perform cloud requests without
interrupting tilt sampling, UART processing, or game controls.

TLS is verified with public roots downloaded from https://pki.goog/roots.pem.
`GoogleRoots.h` contains the same PEM bundle for the Arduino sketch. Update it
from the official source if Google's trust roots change; do not disable TLS checks.

References:
- https://firebase.google.com/docs/reference/rest/auth
- https://firebase.google.com/docs/database/rest/save-data
- https://firebase.google.com/docs/database/security/rules-conditions
