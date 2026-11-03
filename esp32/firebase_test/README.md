# Firebase high-score bridge

Standalone ESP32-S3/S2 test: reads `/skyStrike/highScore`, saves completed-game
scores only when higher, and returns the best score/new-record result to the game.
The game connects through `wss://skystrike-relay.onrender.com`. Only connect one
ESP32 at a time to this relay. Uploading this replaces the firmware on that board;
it does not change the main flight sketch or MCX firmware.

## Setup

1. Firebase Authentication: enable Email/Password and create the device user.
2. Realtime Database > Rules: publish `database.rules.json` with that user's UID.
   App Check enforcement is not supported by this basic REST client.
3. Copy `config.example.h` to `config.h` and enter hotspot credentials, API key,
   and device email/password. URL and UID are already provided. Do not append
   the console's `: null` display to the URL; it means the database is empty.
4. Open `firebase_test.ino` in Arduino IDE. Keep **FirebaseWorker.h**, config.h,
   and GoogleRoots.h in the sketch folder. Select **ESP32S3 Dev Module** for the
   personal board, or **ESP32S2 Dev Module** for the team board, and select its port.
5. Install **ArduinoJson 7** and **WebSockets by Markus Sattler**. Wi-Fi, HTTPS,
   and FreeRTOS support come with the ESP32 board package.
6. Enable hotspot mobile data. Upload, open Serial Monitor/Tera Term at **115200**,
   and reset. Open the local `game/index.html` and click **Connect**; the address
   is prefilled. The Render-hosted game needs redeploying after browser changes.

Expect `[WS] Connected to Render`, `[AUTH] Signed in; device UID matches`, and
`[FIREBASE] Stored high score: ...`. Their order can vary because work is concurrent.
The browser shows the best score on connection and the game-over screen. It
announces `New record!` only after a successful save is confirmed by the ESP32.

## Task ownership and responsiveness

- **firebase_test.ino / Arduino loop:** owns WebSocket callbacks, sends replies,
  checks Wi-Fi transitions, and schedules reconnection. Callbacks parse/validate
  input and enqueue work without waiting for Firebase.
- **FirebaseWorker.h / firebase task:** exclusively owns HTTP/TLS clients, the
  authentication token, pending scores, and the last eight completed outcomes.
- FirebaseWorker.h is now a small adapter to `../shared/FirebaseWorker.h`. The
  combined S2 flight sketch uses that same implementation with its own private
  configuration. Keep the repository's esp32/ directory layout when compiling;
  the worker no longer has a separate implementation per sketch.
- A FreeRTOS score queue (8 entries) passes fixed-size copied messages to the
  worker. A one-entry overwrite queue coalesces best-score requests. A response
  queue (12 entries) passes results back. No String objects or JSON pointers cross
  task boundaries, and neither task touches the other's mutable state.
- The worker checks Wi-Fi/network time every 100 ms. It starts authentication as
  soon as time is valid; the old 60-second time recheck delay is removed. Failed
  authentication and database jobs retry after 10 seconds, without blocking the
  main loop. Authentication is renewed after 50 minutes or HTTP 401.
- HTTPS remains synchronous **inside the worker**, with bounded request timeouts.
  The worker yields and is not pinned to a core, so the design also supports the
  single-core S2. Its stack allocation is 16 KiB (ESP-IDF uses bytes).
- This removes Firebase waits from the loop; Wi-Fi/WebSocket reconnection can
  still take time, and shared CPU/network resources still limit overall latency.
  Physical sensor timing and worker stack headroom must be verified on hardware
  before integration into esp32_flight.

## Scores, retries, and limitations

The worker buffers eight unique pending scores in addition to the input queue.
It rereads Firebase before comparison. Lower/equal scores do not write. Higher
scores use an ETag-conditional PUT: a concurrent update causes reread/retry rather
than overwriting a newer record. Queue-full submissions are logged and the
browser's 10-second retry can submit again later. ROLL/legacy SCORE stay silent.

Completed results are replayable for the last eight games during this boot. A
full response queue retains a processed score in the worker; an unsent WebSocket
reply remains in the main loop until it can be sent. Duplicate completed browser
requests replay their original outcome. Best-score lookups may be coalesced.

Queues/cache are RAM only: resetting loses pending jobs and deduplication history.
A write whose acknowledgement is lost can save the record but lose its new-record
flag on retry; this is not a durable celebration-event delivery system. Closing
the browser before confirmation can lose its pending submission. MCU celebration
events remain separate work. The combined S2 firmware is in ../esp32_flight/;
its README lists upload steps and the remaining hardware/browser compatibility
checks. Do not run both firmwares as ESP32 clients on the same relay at once.

## Hardware verification

1. Reset with the browser already connected: best score should load once time
   sync and authentication complete, without the former fixed one-minute delay.
2. Finish with a higher score: verify `New high score saved`, Firebase's Data tab,
   and the browser's `New record!` message.
3. Finish with a lower/equal score: database stays unchanged and no celebration.
4. Finish before authentication completes: score is queued and processed later.
5. Interrupt mobile internet while leaving hotspot Wi-Fi enabled. Submit a score:
   main-loop WebSocket handling must continue independently of blocked HTTPS.
   Restore internet and verify retry/result. A lost relay path can of course
   prevent new browser messages reaching the ESP32 during the outage.
6. Disconnect/reconnect the browser/relay and check pending results recover without
   duplicate writes. Reset and confirm the stored record persists.
7. Check sustained play/reconnects for resets, stack overflow, or low-memory issues
   before porting the worker into the main sensor firmware.

## Privacy and troubleshooting

config.h is Git-ignored but manually created ZIPs may include it: exclude it when
sharing/submitting. Never print passwords, tokens, auth responses, or request URLs.
TLS uses Google's public roots in GoogleRoots.h, matching google_roots.pem from
https://pki.goog/roots.pem. Do not disable Firebase certificate checks.

- No IP: hotspot name/password or 2.4 GHz compatibility.
- Waiting for time: mobile internet or NTP access unavailable.
- Auth 400: API key, account credentials, or Email/Password provider.
- UID mismatch: signed into the wrong device account.
- Database 401/403: published rules, token, or App Check configuration.
- Negative HTTP result: network/TLS failure; check clock and internet.
- `[FATAL]`: queue/task allocation failed; do not continue testing that boot.

References:
- https://firebase.google.com/docs/reference/rest/auth
- https://firebase.google.com/docs/database/rest/save-data
- https://firebase.google.com/docs/database/security/rules-conditions
