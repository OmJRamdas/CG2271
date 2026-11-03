# ESP32-S2 flight controller with Firebase high scores

This is the combined firmware: MPU/joystick/fire inputs, MCX UART link, Render
WebSocket relay, and the tested Firebase high-score worker run on the same S2.
Upload this sketch instead of firebase_test when using the real controller.

## Prepare and upload

1. Keep the repository's `esp32/` layout. `esp32_flight/FirebaseWorker.h` includes
   the common implementation and public trust roots in `esp32/shared/`.
   The standalone firebase_test uses that same worker rather than a separate copy.
2. Copy config.example.h to **config.h** in this folder. Enter hotspot credentials,
   Firebase API key, device account email/password, database URL and UID. The format
   matches firebase_test/config.h, so an existing private test configuration can
   be copied here locally. The two config.h files are both Git-ignored; exclude
   them manually from source ZIPs too.
3. Install ArduinoJson 7, WebSockets by Markus Sattler, Adafruit Unified Sensor
   and Adafruit BusIO. (Adafruit MPU6050 is not needed: `MpuRegisters.h` drives
   the MPU directly so clone chips with WHO_AM_I 0x70 work.) Select **ESP32S2 Dev Module** and
   the actual controller's port. Compile/upload esp32_flight.ino.
4. Default relay settings are SSL enabled, `skystrike-relay.onrender.com`, port
   443, `/`. For a local relay, adjust WS_USE_SSL/WS_HOST/WS_PORT in the sketch.
5. Wire sensors and MCX using the root README pin table. Use 115200 for debug
   Serial, and 9600 for the inter-board UART. Keep the MPU still for calibration.
6. Open the updated game, connect to `wss://skystrike-relay.onrender.com`, and check
   the best score and game-over save result. The older hosted game needs redeploying
   to include the current submit_score/high-score display code.

Only this combined board should identify as `esp32` on the relay. Disconnect the
personal S3 test board during integrated testing, since the relay supports one
ESP32 client at a time.

## Ownership and startup

- Main Arduino loop: MPU/I2C at 100 Hz, joystick/button processing, 20 Hz sensor
  JSON, MCX UART, and the sole WebSocket client. It handles Firebase request/reply
  messages using nonblocking queues; it never performs Firebase HTTPS calls.
- Shared firebase task: authentication/renewal, Firebase GET/conditional PUT,
  pending score retries, and cached result replay. It never accesses Wire,
  sensors, MCX state or the WebSocket object. It yields on single-core S2.
- Wi-Fi startup does not wait 15 seconds before starting sensor sampling anymore.
  Network time is synchronized asynchronously; Firebase waits in its own task.
- Score messages do not mark browser game state fresh or reset the MCX watchdog.
  MCX state/events continue to use the existing game_state/event handler.
- If the Firebase task cannot allocate memory, its failure is logged and hardware
  controls continue; cloud scoring remains unavailable until a successful restart.

## Required bench checks

Compile success does not prove hardware timing. On the S2 verify calibration,
100 Hz sample freshness, 20 Hz transmission, quick/held fire, encoder telemetry,
UART ACK/NAK and actuator feedback while a score is being saved. Repeat with
internet loss, relay reconnect, and Firebase retries. TLS and the sensor task
share CPU/heap on a single core; profile real sample timing and task stack/heap
headroom before the demo. WebSocket reconnect/TLS setup can still take time in
the main loop; moving Firebase does not remove every network latency source.

### Browser/MCX compatibility (resolved)

game/game.js was reconciled with this firmware on the `integration-fixes` branch.
It sends the `game_state` heartbeat (lives, score, IDLE/PLAY/OVER) every 400 ms
and on connect, plus the supported START/HIT/BONUS/OVER events (no more
SCORE/ROLL/BOOST). It reads the firmware's `throttle`, `fireSession`/`fireCount`/
`firePending`, `fireHeld` and `tiltValid` fields. Press **I** in the game to see
the live controller inputs and the state last sent to the MCX. High-score
messages still do not keep the MCX watchdog alive on their own; the heartbeat does.

The shared worker's retry/queue/reset limitations and REST message examples are
documented in ../firebase_test/README.md. MCU new-record celebration is not added
by this integration.
