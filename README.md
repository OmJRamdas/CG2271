# CG2271 Assignment 4 — Tilt-Controlled Flight Game

## Problem Statement

A tilt-controlled browser flight game where physical sensors on the ESP32 and MCXC444 drive a Three.js airplane through a cloud-hosted WebSocket relay. The player tilts the MPU-6050 to steer, uses a joystick to orbit the camera, presses a button to fire, and turns a rotary encoder on the MCXC444 to set the throttle. Game events feed back to the MCXC444 to drive LEDs, a buzzer, and a servo.

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
 │    encoderTask(pri 2) — encoder throttle, report       │
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
 │    - Rotary encoder PTA12 D3 / PTA13 D8 (pin IRQ)      │
 └────────────────────────────────────────────────────────┘
```

## Hardware

### Sensors (4 required)

| # | Sensor | Connected to | Interface | Polled / Interrupt | Purpose |
|---|--------|--------------|-----------|--------------------|---------|
| 1 | GY-521 MPU-6050 | ESP32-S2 | I2C (SDA/SCL) | Polled (100 Hz) | Tilt → pitch & roll of airplane |
| 2 | Joystick module | ESP32-S2 | 2× ADC | Polled (ADC) | Camera orbit (X/Y); joystick switch is not connected by this sketch |
| 3 | Push button | ESP32-S2 | GPIO | Interrupt (falling edge) | Fire (one shot per debounced press) |
| 4 | Rotary encoder | MCXC444 | CLK PTA12 (D3), DT PTA13 (D8) | Interrupt (PORTA pin change) | Throttle 0–100 % |

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
| Encoder CLK | PTA12 | D3 | encoder CLK |
| Encoder DT | PTA13 | D8 | encoder DT (module + → 3.3 V, GND → GND, SW unused) |

The pins were chosen from the MCXC444 reference manual's pin-mux table and the FRDM schematic (`mcx/extras/`). The servo is on PTA5 because PTD0 (the old servo pin) isn't on any header; it's wired to the onboard LCD. The encoder uses PTA12/PTA13 because PORTA has a pin-change interrupt (PORTB has none). If the throttle moves the wrong way when you turn the knob, set `ENC_REVERSE true`.

## ESP32 tilt integration and controls

Open `esp32/esp32_flight/esp32_flight.ino` in Arduino IDE and select **ESP32S2 Dev Module**. Keep the `esp32/` folder layout, including the sketch's sensor headers and `shared/` Firebase implementation. Install **WebSockets by Markus Sattler**, **Adafruit MPU6050**, **Adafruit Unified Sensor**, **Adafruit BusIO**, and **ArduinoJson 7** through Library Manager. Copy `esp32/esp32_flight/config.example.h` to private, Git-ignored `config.h` and enter Wi-Fi/Firebase credentials. The default relay is `wss://skystrike-relay.onrender.com`; enter that same URL in the browser. Full upload and bench-test instructions are in [the integrated flight sketch guide](esp32/esp32_flight/README.md).

Firebase authentication, high-score reads and conditional writes run in a separate FreeRTOS worker. Sensor sampling, UART, and WebSocket ownership stay in the Arduino loop; queues pass score requests/replies between the tasks. Wi-Fi startup no longer waits for a connection before entering the sensor loop. See the integrated guide's browser/MCX compatibility section: the current browser still needs its legacy gameplay messages reconciled with the firmware's game-state heartbeats; high-score messages alone do not keep the MCX watchdog alive.

Wiring on the ESP32: UART TX GPIO1 / RX GPIO2, joystick X GPIO3 / Y GPIO5 (`USE_JOYSTICK 1`), fire button GPIO7 to ground with the internal pull-up, and MPU SDA GPIO8 / SCL GPIO9 at address `0x68`. Use common ground between boards. The rotary encoder is on the MCXC444 (see MCXC444 wiring), which sends its 0–100 level as `$ENC`; the ESP32 forwards it as `throttle` while fresh (≤ 1500 ms) and sends `-1` otherwise, so the browser falls back to Shift/Ctrl. The browser maps a changed level to target speed, and Shift/Ctrl still adjust between knob movements. `EncoderThrottle.h` is no longer used by the sketch. `fireHeld` (`FireHold.h`, 20 ms debounce) makes holding the button fire repeatedly; taps shorter than a packet still count through `fireCount`. Encoder detent size, direction and hold behaviour are untested on hardware.

`TiltController` exclusively configures and samples the MPU. The sketch initializes Wire at 400 kHz with a 20 ms bus timeout. Sampling runs at 100 Hz, independently of 20 Hz sensor JSON transmission. All sensor calls run in the Arduino loop task; if another task later shares Wire, every user of the bus must use the same mutex.

After WiFi startup, hold the controller still, component side up, near level. Calibration needs 250 ms settling and 200 stationary samples (about 2.25 seconds with an uninterrupted loop). Movement restarts the sample window. Calibration records both gyro bias and the neutral pose. Serial at 115200 baud reports calibration, ready, stale, and fault transitions. Send `c` to recalibrate, `r` to recenter a valid reading without remeasuring gyro bias, or `i` to reinitialize after checking/reconnecting the MPU. Recovery is explicit, avoiding repeated blocking initialization attempts on a broken bus.

### Direction compatibility

The sketch sets `pitchSign = -1`, `rollSign = +1`, and `yawSign = +1`. This preserves the original sketch's settled accelerometer directions: positive sensor X gives positive pitch, positive Y gives positive roll, and positive Z rotation gives positive yaw. Independent pitch and roll still reach full input at 45 degrees from neutral; yaw reaches full input at 90 degrees/second. Yaw is rotation speed, so twisting and then holding the controller still returns yaw input to zero. The estimator uses the physically matching gyro axes rather than retaining the old filter's crossed X/Y gyro terms. Near neutral, the module applies a 1.5-degree tilt deadzone and a 3-degree/second yaw deadzone.

The joystick retains its original X/Y directions, 80-count deadzone, and camera-orbit role. Keyboard directions remain unchanged: W/S pitch, A/D roll, Q/E yaw, arrows/mouse camera, Shift/Ctrl throttle, and held Space to fire repeatedly. With fresh sensor packets the physical controller owns pitch/roll/yaw/camera; keyboard throttle and Space remain available. If no sensor packet arrives for 300 ms or the browser WebSocket closes, the browser clears queued controller input and falls back to keyboard/mouse.

The fire button retains falling-edge interrupts and 50 ms debounce. Accepted edges increment a counter under a shared ISR/loop critical section. Packets include `fireSession` (per ESP32 boot), `fireCount` (cumulative accepted edges), `firePending` (edges since the last successful send), and the compatible `fire` boolean. The ESP32 advances its sent counter only after WebSocket send succeeds. The browser queues new presses, ignores duplicate counters, recovers skipped packets within a session, and consumes one press only when a projectile is actually fired. A later `fire:false` packet or an active weapon cooldown cannot erase a queued press. Held Space keeps its original repeated-fire behavior. Presses received outside gameplay are not queued; disconnects, controller timeout, restart, and game over clear pending shots.

### Sensor validity and throttle

`latest()` returns zero controls and `valid=false` during calibration, a sensor fault, freefall, or when the last usable sample is over 150 ms old. These neutral readings are still transmitted alongside joystick/button data. The JSON includes `tiltValid` for the browser. The MPU presence check catches common disconnects, and finite/range checks reject implausible values. Adafruit's `getEvent()` does not expose every underlying I2C read failure: `valid` means a calibrated, plausible, timely reading, not a guarantee that every byte was successfully transported. Verify disconnect behavior on the real wiring.

MCX `$ENC` reports are forwarded as `throttle` (0–100). An encoder report expires after 1500 ms, after which `throttle` is `-1` and the browser keeps its own throttle. The LDR (and the game's stealth mode) has been removed; the browser simply never receives `ldrValid: true`, so stealth stays off.

### Browser/ESP32 messages

- ESP32 → browser: `{"type":"sensor","pitch":0,"roll":0,"yaw":0,"tiltValid":true,"joyX":0,"joyY":0,"fire":false,"fireSession":123,"fireCount":0,"firePending":0,"fireHeld":false,"throttle":50}` (`throttle` is `-1` when no fresh MCX encoder reading).
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
| `$ENC,<seq>,<level 0-100>*CK` | Rotary encoder throttle: immediately on every detent change, and every 500 ms as a heartbeat |

### Flow control

- **Every ESP32 command is acknowledged.** The ESP32 uses stop-and-wait, resending the identical sequence/body on `NAK` or after a 200 ms ACK timeout, with at most three retries. MCX ACK/NAK and encoder telemetry do not themselves need acknowledgments. The MCX re-ACKs an exact retry of its last accepted frame without reapplying actuator effects. A full 16-event queue or an exhausted retry budget increments an ESP32 diagnostic counter (`droppedEvents` / `failedFrames`); the loop continues. Game states are coalesced and sent when changed or due for a 400 ms heartbeat, with pending retries taking priority.
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
| `encoderTask` | 2 | MINIMAL+200 | Woken by the PORTA ISR's notification when the throttle changes (or every 500 ms). Reads the level in a critical section and queues `$ENC` |
| `ledTask` | 1 | MINIMAL+40 | Flash patterns from `ledQueue`; otherwise shows the mode or link state |
| `buzzerTask` | 1 | MINIMAL+40 | Plays an on/off beep pattern per event from `buzzerQueue` on the active buzzer (GPIO; the buzzer makes its own tone) |
| `servoTask` | 1 | MINIMAL+40 | Lives gauge with slew limiting, updated every 50 ms |

### ISRs

Both ISRs are short and never block. They use only FromISR APIs, and the ISR never waits for input.

- **`UART2_FLEXIO_IRQHandler`** (NVIC priority 1)
  - Receive: assembles characters into a line, discards lines that overflow or have UART errors, and hands complete lines over with `xQueueSendFromISR`.
  - Send: while TIE is set, sends `txBuf` one character per TDRE (as in the AS03 pattern), then gives `txDoneSem`.
- **`PORTA_IRQHandler`** (NVIC priority 2): on every encoder pin edge, clears the flag, decodes the quadrature step (`encoder.c`), and wakes `encoderTask` with `vTaskNotifyGiveFromISR` when the throttle level changes.

### Synchronization & resource protection

| Object | Type | Between | Protects / signals |
|---|---|---|---|
| `rxQueue` (5 × 66 B) | Queue | UART ISR → commRxTask | complete received lines |
| `txQueue` (6 × 32 B) | Queue | commRx/encoder → commTxTask | outgoing frames, with UART TX owned by a single task |
| `ledQueue`, `buzzerQueue` | Queue | commRxTask → actuators | event commands |
| `gameStateMutex` | Mutex | commRx (writes) ↔ led/servo (read) | the shared `GameState` struct |
| `printMutex` | Mutex | all tasks that log | the debug console (stops `PRINTF` output interleaving) |
| `txDoneSem` | Binary semaphore | UART ISR → commTxTask | "frame sent", so `txBuf` can be reused |
| task notification | Notification | PORTA ISR → encoderTask | "throttle changed" (lighter than a semaphore) |
| critical section | `taskENTER_CRITICAL` | PORTA ISR ↔ encoderTask | the encoder decoder state |

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

Before a demo, compile/upload for ESP32-S2 with the listed libraries, then verify: calibration while still and while moving; both directions of each axis; no movement at neutral; yaw returning to zero when rotation stops; quick button presses across packet intervals and cooldowns; MPU unplug/reconnect and explicit `i` recovery; browser/relay interruption and keyboard fallback; encoder turns in both directions and stale `$ENC` telemetry; and MCX START/HIT/BONUS/OVER feedback. Confirm a lost ACK does not replay feedback and prolonged cloud loss triggers the MCX watchdog.


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

## Run locally (relay on a laptop)

For bench testing without a deployed relay. The demo itself should use a cloud relay: set `WS_USE_SSL 1` with its `wss://` host on port 443.

1. **Wiring:** boards linked over UART (ESP GPIO1 → PTE23, GPIO2 ← PTE22, common GND). Sensors and actuators are wired as in the tables above.
2. **MCX:** debug `cg2271project_v1` in MCUXpresso, with the terminal at 115200.
   - Expect the controller banner, then the red LED slow-blinking ("link lost") until the ESP32 starts sending.
3. **Network:** turn on a phone hotspot on **2.4 GHz** (iPhone: *Maximize Compatibility*) and connect the laptop to it. Note the laptop's Wireless LAN IPv4 address from `ipconfig`.
4. **Relay:** run `cd relay`, then `npm install` (first time only), then `node server.js`. Allow Node through Windows Firewall on private networks.
5. **ESP32:** set `WIFI_SSID`/`WIFI_PASSWORD` in private `config.h`. In `esp32_flight.ino`, set `WS_HOST` to the laptop's IPv4, `WS_PORT 3000` and `WS_USE_SSL 0`. Upload, then open Serial Monitor at 115200. Firebase still needs internet access even when the relay is local.
   - Keep the board still for about 2 s while it calibrates.
   - The relay window should print `ESP32 registered`.
   - Don't commit your Wi-Fi password.
6. **Game:** open `game/index.html` in a browser (it needs internet, for Three.js). Enter `ws://localhost:3000` and click **Connect**. The relay window should print `Game registered`.
7. **Check both directions:**
   - **ESP32 → game:** tilt steers, the joystick orbits the camera, the button fires, and the encoder knob changes speed.
   - **Game → MCX:** the red LED is solid during play, triple-flashes and beeps on a hit, and fast-blinks at game over.

If the ESP32 keeps printing `[WS] Disconnected`, check `WS_HOST`, the firewall, and whether the hotspot blocks devices from reaching each other.

## Hardware test (wiring check for both boards)

These are two standalone programs that print every sensor reading and actuator step to a serial monitor. Run them before integrating.

**MCXC444:** [mcx/hwtest/source/cg2271_hwtest.c](mcx/hwtest/source/cg2271_hwtest.c)
1. In MCUXpresso, create a new project `cg2271_hwtest` with the same settings as the main one: FRDM-MCXC444, RTOS, heap_4 only, RTOS template, printf redirected to UART.
2. Replace its `source/cg2271_hwtest.c` with the file from the repo, and copy [mcx/source/encoder.c](mcx/source/encoder.c) and `encoder.h` into the same `source/` folder. Copy [mcx/FreeRTOSConfig_Gen.h](mcx/FreeRTOSConfig_Gen.h) over `freertos/freertos-kernel/template/ARM_CM0/FreeRTOSConfig_Gen.h`.
3. Build, debug, and open the MCUXpresso terminal at 115200.
   - **Actuators:** at boot, the LED lights for 1 s, the buzzer beeps 3 times, and the servo sweeps 0° → 90° → 180° → 90°, with each step printed. Press **SW3** to run it again.
   - **Encoder:** each detent prints `[ENC]` with the level moving by 5 %. If it moves the wrong way, set `ENC_REVERSE true`; if one click moves it by 10 % or needs two clicks, the knob has a different detent size. Every 5 s a summary shows how many edges were seen (0 edges = wiring fault).
   - **Link:** `[LINK] rx: PING,n` lines appear once the ESP32 test is running and wired up.

**ESP32-S2:** [esp32/hw_test/hw_test.ino](esp32/hw_test/hw_test.ino), which needs no extra libraries. Upload it with the board set to *ESP32S2 Dev Module*, then open Serial Monitor at 115200.
- **At startup:**
  - The I2C scan should list `0x68`, followed by `[MPU] WHO_AM_I ... PASS`.
  - The joystick's resting centre should be about 2000 on each axis.
- **Every 200 ms**, a status line prints, e.g. `MPU pitch= +12.3 roll= -4.1 | JOY x=2051 y=1987 | BTN presses=3 | LINK sent=12 ok=12 lost=0 rtt=18ms`.
  - Tilting changes pitch and roll.
  - The joystick should span roughly 0–4095 on each axis.
  - Each button press counts up.
- **With both boards wired** (ESP GPIO1 → PTE23, GPIO2 ← PTE22, common GND), `ok` counts up and `lost` stays at 0. If pings are lost, it prints a wiring hint.

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
| 4 sensors | ✅ | MPU-6050, joystick, button, rotary encoder |
| 3 actuators | ✅ | Red LED, buzzer, servo |
| ≥1 sensor on MCXC444 | ✅ | Rotary encoder on MCXC444 PTA12/PTA13 |
| ≤1 onboard actuator | ✅ | Red LED only; buzzer + servo external |
| Polling + interrupt | ✅ | Polled: the ESP32's MPU-6050 and joystick. Interrupt-driven: the MCX rotary encoder (PORTA ISR) and the ESP32's button |
| ≥1 ISR | ✅ | UART2 ISR (RX + TX) and PORTA encoder ISR on the MCXC444 |
| ≥3 tasks | ✅ | 6 tasks |
| ≥1 mutex/semaphore | ✅ | gameStateMutex, printMutex, txDoneSem (binary) |
| ≥1 queue | ✅ | rxQueue, txQueue, ledQueue, buzzerQueue |
| Time-slicing | ✅ | `configUSE_TIME_SLICING 1`; led/buzzer/servo share priority 1 |
| Pre-emption | ✅ | `configUSE_PREEMPTION 1`; commRxTask (priority 3) pre-empts the other tasks |
| Bi-directional UART | ✅ | ESP32→MCX: `$GS`/`$EV`. MCX→ESP32: `$ENC` and `$ACK`/`$NAK`. Frames are checksummed, sequenced and acknowledged |
| Cloud/internet | ✅ | ESP32 WebSocket client → cloud relay |
| Non-trivial online | ✅ | Real-time game data exchange |
| Coherent product | ✅ | All sensors/actuators serve the flight game |

## Work Breakdown (5 Members)

### Stream 1 — ESP32 Sensor Hub
- Wire and read MPU-6050 over I2C (pitch, roll, yaw).
- Wire joystick (2× ADC for camera orbit).
- Wire push button with debounce.
- Package sensor readings into JSON for WebSocket.
- UART to MCXC444 with checksummed state/events, ACK/NAK retries and encoder throttle reception.

### Stream 2 — ESP32 WiFi + WebSocket Client
- Connect to phone hotspot via WiFi.h.
- Establish WebSocket client to cloud relay (use WebSockets by Markus Sattler).
- Sample tilt at 100 Hz and send sensor JSON at 20 Hz.
- Receive game event JSON, parse, forward over UART.
- Handle reconnection on WiFi drop.

### Stream 3 — MCXC444 FreeRTOS Firmware
- ✅ UART RX and TX, both interrupt-driven, using the checksummed frame protocol (`$GS`/`$EV` in, `$ACK`/`$NAK`/`$ENC` out).
- ✅ Rotary encoder: PORTA pin-change ISR with a quadrature decoder (`encoder.c`, host unit-tested), reported as `$ENC`.
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
- Map persistent sensor data to pitch/roll/yaw, camera orbit, fire, and encoder throttle; keep keyboard throttle as fallback.
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
│   │   ├── encoder.c/.h       ← rotary encoder decoder (hardware-independent)
│   ├── FreeRTOSConfig_Gen.h   ← kernel config (time-slicing etc.)
│   ├── tests/                 ← host unit tests (test_protocol.c, test_encoder.c)
│   ├── tools/esp32_sim.py     ← ESP32 simulator for standalone testing
│   └── extras/                ← reference manual, schematic, lab templates
├── esp32/
│   ├── esp32_flight/
│   │   ├── esp32_flight.ino   ← Arduino sketch
│   │   ├── TiltController.h   ← MPU sampling, calibration, filtering
│   │   ├── ButtonEvents.h     ← debounced press counter
│   │   └── McxLink.h          ← UART framing, retries, $ENC parser
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
