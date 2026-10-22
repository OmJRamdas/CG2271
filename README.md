# CG2271 Assignment 4 — Tilt-Controlled Flight Game

## Problem Statement

A tilt-controlled browser flight game where physical sensors on the ESP32 and MCXC444 drive a Three.js airplane through a cloud-hosted WebSocket relay. The player tilts the MPU-6050 to steer, uses a joystick to orbit the camera, presses a button to fire, and covers a light sensor for stealth mode. Game events feed back to the MCXC444 to drive LEDs, a buzzer, and a servo.

## Architecture

```
 ┌────────────────────────────────────────────────────────┐
 │                    PLAYER'S BROWSER                    │
 │             Three.js flight game (HTML/JS)             │
 │         hosted on GitHub Pages / Netlify / etc.        │
 └──────────────────────┬─────────────────────────────────┘
                        │ WebSocket (wss://)
                        ▼
 ┌────────────────────────────────────────────────────────┐
 │              CLOUD RELAY SERVER (Node.js)              │
 │       hosted on Glitch / Render / Railway (free)       │
 │  - Relays sensor JSON from ESP32 → browser            │
 │  - Relays game events from browser → ESP32            │
 └──────────────────────┬─────────────────────────────────┘
                        │ WebSocket (wss://)
                        ▼
 ┌────────────────────────────────────────────────────────┐
 │                   ESP32-S2 (Arduino)                   │
 │  Sensors (I2C / GPIO / ADC):                          │
 │    - MPU-6050 (pitch & roll via I2C)                  │
 │    - Joystick (X/Y analog)                            │
 │    - Push button (GPIO interrupt)                     │
 │  WiFi: connects to phone hotspot                     │
 │  WebSocket client → cloud relay                       │
 │  UART TX → MCXC444    UART RX ← MCXC444              │
 └──────────────┬────────────────┬────────────────────────┘
           TX (PTE22)       RX (PTE23)
                │                │
 ┌──────────────▼────────────────▼────────────────────────┐
 │              FRDM-MCXC444 (FreeRTOS)                   │
 │                                                        │
 │  Tasks:                                                │
 │    commRxTask (pri 3) — parse frames, ACK/NAK, state   │
 │    commTxTask (pri 2) — sole owner of UART TX          │
 │    ldrTask    (pri 2) — LDR ADC polling, report        │
 │    ledTask    (pri 1) — red LED patterns               │
 │    buzzerTask (pri 1) — buzzer beep patterns (GPIO)    │
 │    servoTask  (pri 1) — lives gauge (TPM0 PWM)         │
 │                                                        │
 │  Sync:                                                 │
 │    Queues: rxQueue, txQueue, ledQueue, buzzerQueue     │
 │    Mutex:  gameStateMutex, printMutex                  │
 │    Binary semaphore: txDoneSem                         │
 │                                                        │
 │  Actuators:                                            │
 │    - Red LED    PTE31  (onboard, active-LOW)           │
 │    - Buzzer     PTD4   D10 (external, active, GPIO)    │
 │    - Servo SG90 PTA5   D5 (external, PWM)              │
 │                                                        │
 │  Sensor:                                               │
 │    - LDR (photoresistor) PTB0 A0 (ADC0_SE8)            │
 └────────────────────────────────────────────────────────┘
```

## Hardware

### Sensors (4 required)

| # | Sensor | Connected to | Interface | Polled / Interrupt | Purpose |
|---|--------|--------------|-----------|--------------------|---------|
| 1 | GY-521 MPU-6050 | ESP32-S2 | I2C (SDA/SCL) | Polled (100 Hz) | Tilt → pitch & roll of airplane |
| 2 | Joystick module | ESP32-S2 | 2× ADC | Polled (ADC) | Camera orbit (X/Y); joystick switch is not connected by this sketch |
| 3 | Push button | ESP32-S2 | GPIO | Interrupt (falling edge) | Fire (one shot per debounced press) |
| 4 | LDR (photoresistor) | MCXC444 | ADC0_SE8, PTB0 (A0) | Polled (ldrTask, 100 ms) | Stealth indicator and dimmed aircraft navigation lights |

### Actuators (3 required)

| # | Actuator | Connected to | Pin | Type | Purpose |
|---|----------|--------------|-----|------|---------|
| 1 | Red LED | MCXC444 | PTE31 (onboard) | GPIO, active-LOW | Game mode / hit / link-lost indicator |
| 2 | Active buzzer | MCXC444 | PTD4, header D10 | GPIO, active-HIGH | A beep pattern per game event |
| 3 | Servo SG90 | MCXC444 | PTA5, header D5 | TPM0_CH2 PWM, 50 Hz | Lives gauge (0 lives = 0°, 3 = 180°) |

### MCXC444 wiring

| Signal | MCX pin | Header | Connect to |
|---|---|---|---|
| UART2 TX | PTE22 | — | ESP32 Serial1 RX |
| UART2 RX | PTE23 | — | ESP32 Serial1 TX |
| GND | GND | — | ESP32 GND (common ground is required) |
| Servo signal | PTA5 | D5 | SG90 orange wire (power the servo from 5 V, GND shared) |
| Buzzer signal | PTD4 | D10 | buzzer S pin (VCC → 3.3 V, GND → GND) |
| LDR | PTB0 | A0 | 3.3 V — LDR — A0 — 10 kΩ — GND |

The pins were chosen from the MCXC444 reference manual's pin-mux table and the FRDM schematic (`mcx/extras/`). The servo is on PTA5 because PTD0 (the old servo pin) isn't on any header; it's wired to the onboard LCD. PTB0 also carries the MCU-Link I2C clock. If the LDR readings are noisy, move it to PTB2 (header A2) by setting `LDR_PIN 2U` and `LDR_ADC_CH 12U`.

## ESP32 tilt integration and controls

Open `esp32/esp32_flight/esp32_flight.ino` in Arduino IDE and select **ESP32S2 Dev Module**. Keep `TiltController.h`, `McxLink.h`, and `ButtonEvents.h` in the same sketch directory. Install **WebSockets by Markus Sattler**, **Adafruit MPU6050**, **Adafruit Unified Sensor**, **Adafruit BusIO**, and **ArduinoJson 7** through Library Manager. Set the sketch's WiFi credentials and relay host/port/path before uploading; enter the same relay's `wss://` URL in the browser.

Wiring on the ESP32: UART TX GPIO1 / RX GPIO2, rotary encoder CLK GPIO6 / DT GPIO5 (module + to 3.3 V, SW unused), fire button GPIO7 to ground with the internal pull-up, and MPU SDA GPIO8 / SCL GPIO9 at address `0x68`. Use common ground between boards. The joystick is disabled by default (`USE_JOYSTICK 0`) because it shares GPIO5 with the encoder, so `joyX`/`joyY` are sent as 0. The encoder is decoded by pin-change interrupts into a 0-100 throttle level (`EncoderThrottle.h`); the browser maps a changed level to target speed, and Shift/Ctrl still adjust between knob movements. `fireHeld` (`FireHold.h`, 20 ms debounce) makes holding the button fire repeatedly; taps shorter than a packet still count through `fireCount`. Encoder detent size, direction and hold behaviour are untested on hardware.

`TiltController` exclusively configures and samples the MPU. The sketch initializes Wire at 400 kHz with a 20 ms bus timeout. Sampling runs at 100 Hz, independently of 20 Hz sensor JSON transmission. All sensor calls run in the Arduino loop task; if another task later shares Wire, every user of the bus must use the same mutex.

After WiFi startup, hold the controller still, component side up, near level. Calibration needs 250 ms settling and 200 stationary samples (about 2.25 seconds with an uninterrupted loop). Movement restarts the sample window. Calibration records both gyro bias and the neutral pose. Serial at 115200 baud reports calibration, ready, stale, and fault transitions. Send `c` to recalibrate, `r` to recenter a valid reading without remeasuring gyro bias, or `i` to reinitialize after checking/reconnecting the MPU. Recovery is explicit, avoiding repeated blocking initialization attempts on a broken bus.

### Direction compatibility

The sketch sets `pitchSign = -1`, `rollSign = +1`, and `yawSign = +1`. This preserves the original sketch's settled accelerometer directions: positive sensor X gives positive pitch, positive Y gives positive roll, and positive Z rotation gives positive yaw. Independent pitch and roll still reach full input at 45 degrees from neutral; yaw reaches full input at 90 degrees/second. Yaw is rotation speed, so twisting and then holding the controller still returns yaw input to zero. The estimator uses the physically matching gyro axes rather than retaining the old filter's crossed X/Y gyro terms. Near neutral, the module applies a 1.5-degree tilt deadzone and a 3-degree/second yaw deadzone.

The joystick retains its original X/Y directions, 80-count deadzone, and camera-orbit role. Keyboard directions remain unchanged: W/S pitch, A/D roll, Q/E yaw, arrows/mouse camera, Shift/Ctrl throttle, and held Space to fire repeatedly. With fresh sensor packets the physical controller owns pitch/roll/yaw/camera; keyboard throttle and Space remain available. If no sensor packet arrives for 300 ms or the browser WebSocket closes, the browser clears queued controller input and falls back to keyboard/mouse.

The fire button retains falling-edge interrupts and 50 ms debounce. Accepted edges increment a counter under a shared ISR/loop critical section. Packets include `fireSession` (per ESP32 boot), `fireCount` (cumulative accepted edges), `firePending` (edges since the last successful send), and the compatible `fire` boolean. The ESP32 advances its sent counter only after WebSocket send succeeds. The browser queues new presses, ignores duplicate counters, recovers skipped packets within a session, and consumes one press only when a projectile is actually fired. A later `fire:false` packet or an active weapon cooldown cannot erase a queued press. Held Space keeps its original repeated-fire behavior. Presses received outside gameplay are not queued; disconnects, controller timeout, restart, and game over clear pending shots.

### Sensor validity and LDR

`latest()` returns zero controls and `valid=false` during calibration, a sensor fault, freefall, or when the last usable sample is over 150 ms old. These neutral readings are still transmitted alongside joystick/button data. The JSON includes `tiltValid` for the browser. The MPU presence check catches common disconnects, and finite/range checks reject implausible values. Adafruit's `getEvent()` does not expose every underlying I2C read failure: `valid` means a calibrated, plausible, timely reading, not a guarantee that every byte was successfully transported. Verify disconnect behavior on the real wiring.

MCX `$LDR` reports are forwarded as `ldrRaw`, `dark`, and `ldrValid`. An LDR report expires after 1000 ms. Fresh darkness displays STEALTH and dims the player's red/green navigation lamps to 15%; stale or invalid LDR data restores normal lamps. The current MCX has an LDR, so there is no potentiometer throttle field; throttle remains on Shift/Ctrl.

### Browser/ESP32 messages

- ESP32 → browser: `{"type":"sensor","pitch":0,"roll":0,"yaw":0,"tiltValid":true,"joyX":0,"joyY":0,"fire":false,"fireSession":123,"fireCount":0,"firePending":0,"fireHeld":false,"throttle":50,"ldrValid":true,"ldrRaw":1500,"dark":false}`.
- Browser → ESP32: `{"type":"game_state","lives":3,"score":0,"mode":"PLAY"}` every 400 ms, including on the start screen (`IDLE`) and after game over (`OVER`). The 100-point hull is preserved; the servo's three segments use `ceil(health * 3 / 100)` (100–67 → 3, 66–34 → 2, 33–1 → 1, 0 → 0).
- Browser → ESP32: `{"type":"event","event":"HIT","value":1}` for player damage; `START` at start/restart; `OVER` at game over; `BONUS` on a subsequent wave. Enemy kills update score/state, rather than triggering player-hit feedback. State is sent before each event. Legacy `ROLL`, `SCORE`, and `BOOST` UART commands are no longer used.

The relay forwards these JSON messages unchanged. ESP32 parsing uses the WebSocket payload length and validates game-state fields before forwarding. State older than 1500 ms, or an ESP32 WebSocket disconnect, suspends UART state/events so the MCX's watchdog can expose the lost browser/cloud link. Reconnection resumes from fresh state instead of replaying queued stale events.

## UART Protocol (Bi-directional, 9600 baud 8N1)

Source of truth: [mcx/source/protocol.h](mcx/source/protocol.h). One ASCII frame per line:

```
$<TYPE>,<seq>,<fields...>*<CK>\n
```

- `seq` is 0–65535. The sender increments it for every frame and wraps to 0.
- `CK` is two hex digits: the XOR of every byte between `$` and `*` (the same scheme as NMEA). Example: `$ACK,12*66`.
- Frames are at most 64 characters, not counting `\n`. A trailing `\r` is tolerated.

### ESP32 → MCXC444 (game state from the cloud)

| Frame | Meaning |
|---|---|
| `$GS,<seq>,<lives 0-99>,<score 0-9999999>,<IDLE\|PLAY\|OVER>*CK` | Current game state. Send at least every 500 ms (it also serves as the heartbeat). |
| `$EV,<seq>,<HIT\|START\|OVER\|BONUS>*CK` | One-off game event. Send it once, when it happens. |

### MCXC444 → ESP32

| Frame | Meaning |
|---|---|
| `$ACK,<seq>*CK` | Frame `<seq>` was valid and has been applied |
| `$NAK,<seq>*CK` | Frame `<seq>` was rejected (bad checksum, bad format or out-of-range value). The ESP32 can resend it. `seq` is 0 if it couldn't be read. |
| `$LDR,<seq>,<raw 0-4095>,<dark 0/1>*CK` | Light sensor reading: every 200 ms, and immediately when dark/light changes |

### Flow control

- **Every ESP32 command is acknowledged.** The ESP32 uses stop-and-wait, resending the identical sequence/body on `NAK` or after a 200 ms ACK timeout, with at most three retries. MCX ACK/NAK and LDR telemetry do not themselves need acknowledgments. The MCX re-ACKs an exact retry of its last accepted frame without reapplying actuator effects. A full 16-event queue or an exhausted retry budget increments an ESP32 diagnostic counter (`droppedEvents` / `failedFrames`); the loop continues. Game states are coalesced and sent when changed or due for a 400 ms heartbeat, with pending retries taking priority.
- **Link watchdog:** if the MCX gets no valid frame for 2 s, it marks the link as lost and the red LED slow-blinks. The link recovers automatically on the next valid frame.

### What the MCXC444 does with it

| Input | Red LED | Buzzer | Servo |
|---|---|---|---|
| mode `IDLE` | short blip every 2 s | — | follows lives |
| mode `PLAY` | solid on | — | follows lives |
| mode `OVER` | 5 Hz blink | — | 0° |
| `EV HIT` | triple flash | two short beeps | — |
| `EV BONUS` | double flash | two quick chirps | — |
| `EV START` / `EV OVER` | mode changes immediately | three rising beeps / one long beep | — |
| link lost (2 s silence) | 1 Hz blink | — | holds position |

## FreeRTOS Design (MCXC444)

Code: [mcx/source/cg2271project_v1.c](mcx/source/cg2271project_v1.c). The kernel config is [mcx/FreeRTOSConfig_Gen.h](mcx/FreeRTOSConfig_Gen.h): copy it into the MCUXpresso project's `freertos/freertos-kernel/template/ARM_CM0/`.

### Tasks

| Task | Priority | Stack (words) | Role |
|------|----------|-------|------|
| `commRxTask` | 3 | MINIMAL+200 | Parses and validates frames from `rxQueue`; ACKs or NAKs each one; updates `GameState` under the mutex; dispatches events to the actuator tasks; runs the 2 s link watchdog |
| `commTxTask` | 2 | MINIMAL+80 | The only task that writes to UART2: takes a frame from `txQueue`, starts the interrupt-driven send, then waits on `txDoneSem` |
| `ldrTask` | 2 | MINIMAL+200 | Every 100 ms: starts an ADC conversion and waits for the ISR's notification. Applies a 4-sample moving average and dark/light hysteresis, then queues `$LDR` |
| `ledTask` | 1 | MINIMAL+40 | Flash patterns from `ledQueue`; otherwise shows the mode or link state |
| `buzzerTask` | 1 | MINIMAL+40 | Plays an on/off beep pattern per event from `buzzerQueue` on the active buzzer (GPIO; the buzzer makes its own tone) |
| `servoTask` | 1 | MINIMAL+40 | Lives gauge with slew limiting, updated every 50 ms |

### ISRs

Both ISRs are short and never block. They use only FromISR APIs, and the ISR never waits for input.

- **`UART2_FLEXIO_IRQHandler`** (NVIC priority 1)
  - Receive: assembles characters into a line, discards lines that overflow or have UART errors, and hands complete lines over with `xQueueSendFromISR`.
  - Send: while TIE is set, sends `txBuf` one character per TDRE (as in the AS03 pattern), then gives `txDoneSem`.
- **`ADC0_IRQHandler`** (NVIC priority 2): reads the conversion result and wakes `ldrTask` with `vTaskNotifyGiveFromISR`.

### Synchronization & resource protection

| Object | Type | Between | Protects / signals |
|---|---|---|---|
| `rxQueue` (5 × 66 B) | Queue | UART ISR → commRxTask | complete received lines |
| `txQueue` (6 × 32 B) | Queue | commRx/ldr → commTxTask | outgoing frames, with UART TX owned by a single task |
| `ledQueue`, `buzzerQueue` | Queue | commRxTask → actuators | event commands |
| `gameStateMutex` | Mutex | commRx (writes) ↔ led/servo (read) | the shared `GameState` struct |
| `printMutex` | Mutex | all tasks that log | the debug console (stops `PRINTF` output interleaving) |
| `txDoneSem` | Binary semaphore | UART ISR → commTxTask | "frame sent", so `txBuf` can be reused |
| task notification | Notification | ADC ISR → ldrTask | "conversion done" (lighter than a semaphore) |

### RTOS configuration (`FreeRTOSConfig_Gen.h`)

- **Scheduling:** `configUSE_PREEMPTION 1` and `configUSE_TIME_SLICING 1`. `led`, `buzzer` and `servo` share priority 1 and round-robin. `commRxTask` (priority 3) pre-empts everything as soon as the ISR queues a line.
- **Tick:** `configTICK_RATE_HZ 1000`, a 1 ms tick so `pdMS_TO_TICKS` delays are exact.
- **Removed (unused):** software timers and the timer task, recursive mutexes, counting semaphores, the trace facility, the queue registry, and the `vTaskDelete`, `vTaskPrioritySet`, `uxTaskPriorityGet`, `xTaskResumeFromISR` and `xTimerPendFunctionCall` APIs.
- **Added (safety):** `configCHECK_FOR_STACK_OVERFLOW 2` and `configUSE_MALLOC_FAILED_HOOK 1`. Both hooks stop the system with the red LED solid on.

### Testing without hardware

From the repo root on Windows with Node.js, Python, and GCC/G++ on PATH:

```powershell
./tools/test.ps1
```

This runs the existing MCX parser suite, ESP32/MCX interoperability checks against the real C parser (including checksums, ACK/NAK, lost-ACK duplicates, bounded retries, queue limits, and sequence wrap), tilt/button tests using deterministic sensor/clock substitutes, browser input/shot/state tests using the production JavaScript with a rendering substitute, and the UART simulator self-test. Outputs go into ignored `.build/`. Sensor substitutes test algorithm and integration behavior; they do not replace an ESP32 firmware build or real I2C testing.

Before a demo, compile/upload for ESP32-S2 with the listed libraries, then verify: calibration while still and while moving; both directions of each axis; no movement at neutral; yaw returning to zero when rotation stops; quick button presses across packet intervals and cooldowns; MPU unplug/reconnect and explicit `i` recovery; browser/relay interruption and keyboard fallback; LDR cover/uncover and stale telemetry; and MCX START/HIT/BONUS/OVER feedback. Confirm a lost ACK does not replay feedback and prolonged cloud loss triggers the MCX watchdog.


```sh
# protocol unit tests (any host gcc, e.g. MSYS2)
cd mcx/tests && gcc -std=c11 -Wall -Wextra -I../source test_protocol.c ../source/protocol.c -o test_protocol && ./test_protocol

# ESP32 simulator against the real board (USB-UART adapter on PTE22/PTE23/GND)
py -m venv .venv && .venv\Scripts\python -m pip install pyserial
.venv\Scripts\python mcx\tools\esp32_sim.py --list
.venv\Scripts\python mcx\tools\esp32_sim.py COM7               # scripted game
.venv\Scripts\python mcx\tools\esp32_sim.py COM7 --corrupt 3   # every 3rd frame corrupted, expect NAK
.venv\Scripts\python mcx\tools\esp32_sim.py COM7 --interactive
```

## Cloud / Online Function

### Architecture

1. **Cloud relay server** — a lightweight Node.js WebSocket server (~30 lines) hosted free on Glitch/Render/Railway.
2. **ESP32** connects to phone hotspot WiFi, then opens a WebSocket client connection to the cloud relay.
3. **Browser game** (hosted on GitHub Pages / Netlify) also connects to the same cloud relay.
4. Sensor data flows: ESP32 → relay → browser. Game events flow: browser → relay → ESP32.

### Why This Satisfies the Requirement

- ESP32 connects to the **internet** (not just local network).
- Exchanges data with a **cloud-based web application** (the relay server).
- The game itself is hosted online.
- Non-trivial: real-time bidirectional game data, not just a ping.

### Bonus Marks Opportunity

- Add a REST API call from ESP32 (e.g., OpenWeatherMap → wind effects in game).
- Or query a generative AI API for dynamic mission briefings.

## Requirements Checklist

| Requirement | Satisfied | How |
|-------------|-----------|-----|
| 4 sensors | ✅ | MPU-6050, joystick, button, LDR |
| 3 actuators | ✅ | Red LED, buzzer, servo |
| ≥1 sensor on MCXC444 | ✅ | LDR on MCXC444 ADC |
| ≤1 onboard actuator | ✅ | Red LED only; buzzer + servo external |
| Polling + interrupt | ✅ | Polled: LDR (ldrTask, 100 ms) and the ESP32's MPU-6050. Interrupt-driven: the ESP32's button/touch sensor |
| ≥1 ISR | ✅ | UART2 ISR (RX + TX) and ADC0 ISR on the MCXC444 |
| ≥3 tasks | ✅ | 6 tasks |
| ≥1 mutex/semaphore | ✅ | gameStateMutex, printMutex, txDoneSem (binary) |
| ≥1 queue | ✅ | rxQueue, txQueue, ledQueue, buzzerQueue |
| Time-slicing | ✅ | `configUSE_TIME_SLICING 1`; led/buzzer/servo share priority 1 |
| Pre-emption | ✅ | `configUSE_PREEMPTION 1`; commRxTask (priority 3) pre-empts the other tasks |
| Bi-directional UART | ✅ | ESP32→MCX: `$GS`/`$EV`. MCX→ESP32: `$LDR` and `$ACK`/`$NAK`. Frames are checksummed, sequenced and acknowledged |
| Cloud/internet | ✅ | ESP32 WebSocket client → cloud relay |
| Non-trivial online | ✅ | Real-time game data exchange |
| Coherent product | ✅ | All sensors/actuators serve the flight game |

## Work Breakdown (5 Members)

### Stream 1 — ESP32 Sensor Hub
- Wire and read MPU-6050 over I2C (pitch, roll, yaw).
- Wire joystick (2× ADC for camera orbit).
- Wire push button with debounce.
- Package sensor readings into JSON for WebSocket.
- UART to MCXC444 with checksummed state/events, ACK/NAK retries and LDR reception.

### Stream 2 — ESP32 WiFi + WebSocket Client
- Connect to phone hotspot via WiFi.h.
- Establish WebSocket client to cloud relay (use WebSockets by Markus Sattler).
- Sample tilt at 100 Hz and send sensor JSON at 20 Hz.
- Receive game event JSON, parse, forward over UART.
- Handle reconnection on WiFi drop.

### Stream 3 — MCXC444 FreeRTOS Firmware
- ✅ UART RX and TX, both interrupt-driven, using the checksummed frame protocol (`$GS`/`$EV` in, `$ACK`/`$NAK`/`$LDR` out).
- ✅ LDR polling task with an interrupt-driven ADC, smoothing and hysteresis.
- ✅ LED, buzzer (GPIO beep patterns) and servo (TPM0 lives gauge) tasks.
- ✅ gameStateMutex, printMutex, txDoneSem, 4 queues; time-slicing and pre-emption on; unused kernel features off.
- ✅ Host unit tests (`mcx/tests`) and the ESP32 simulator (`mcx/tools/esp32_sim.py`).
- ⏳ On-hardware verification with the simulator, then integration with the ESP32.

### Stream 4 — Cloud Relay Server
- Build Node.js WebSocket relay (~30 lines, using `ws` library).
- Deploy to Glitch / Render (free tier).
- Relay: ESP32 messages → browser, browser messages → ESP32.
- Optional: add REST API endpoint for high scores or weather.

### Stream 5 — Browser Flight Game
- Three.js scene: sky, terrain, airplane model.
- WebSocket client connects to cloud relay.
- Map persistent sensor data to pitch/roll/yaw, camera orbit, fire, and LDR stealth; retain keyboard throttle.
- Send 400 ms game-state heartbeats and START/HIT/BONUS/OVER events back.
- Host on GitHub Pages or Netlify.

## Timeline

| Date | Milestone |
|------|-----------|
| Week 10 (now) | Finalize design, assign streams, start wiring |
| Fri 23 Oct 2026 | **Interim report due** (2-page PDF + source zip) |
| Week 11–12 | Independent stream development |
| Week 12 | Integration: ESP32 ↔ relay ↔ game ↔ MCXC444 |
| Week 13 lab | **Project demo** (5–7 min) |
| Fri 13 Nov 2026 | **Final report due** (5–10 page PDF + source zip) |

## Repo Structure (Planned)

```
CG2271/
├── README.md                  ← this file
├── mcx/
│   ├── source/                ← copy into the MCUXpresso project's source/
│   │   ├── cg2271project_v1.c ← FreeRTOS firmware (tasks, ISRs, drivers)
│   │   ├── protocol.c/.h      ← UART frame protocol (hardware-independent)
│   ├── FreeRTOSConfig_Gen.h   ← kernel config (time-slicing etc.)
│   ├── tests/test_protocol.c  ← host unit tests
│   ├── tools/esp32_sim.py     ← ESP32 simulator for standalone testing
│   └── extras/                ← reference manual, schematic, lab templates
├── esp32/
│   ├── esp32_flight/
│   │   ├── esp32_flight.ino   ← Arduino sketch
│   │   ├── TiltController.h   ← MPU sampling, calibration, filtering
│   │   ├── ButtonEvents.h     ← debounced press counter
│   │   └── McxLink.h          ← UART framing, retries, LDR parser
│   └── tests/                ← host sensor/button/UART tests
├── relay/
│   ├── server.js              ← WebSocket relay
│   └── package.json
├── game/
│   ├── index.html             ← Three.js flight game
│   ├── config.js / world.js / enemies.js
│   ├── tests/input.test.js    ← controller and game-state tests
│   └── game.js
├── tools/test.ps1             ← combined host checks
└── docs/
    ├── interim_report.pdf
    └── final_report.pdf
```
