# CG2271 Assignment 4 — Tilt-Controlled Flight Game

## Problem Statement

A tilt-controlled browser flight game where physical sensors on the ESP32 and MCXC444 drive a Three.js airplane through a cloud-hosted WebSocket relay. The player tilts the MPU-6050 to steer, uses a joystick for throttle, presses a button to fire, and covers a light sensor for stealth mode. Game events feed back to the MCXC444 to drive LEDs, a buzzer, and a servo.

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
 │    - Joystick (X/Y analog + button)                   │
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
 │    buzzerTask (pri 1) — buzzer tunes (TPM1 PWM)        │
 │    servoTask  (pri 1) — lives gauge (TPM0 PWM)         │
 │                                                        │
 │  Sync:                                                 │
 │    Queues: rxQueue, txQueue, ledQueue, buzzerQueue     │
 │    Mutex:  gameStateMutex, printMutex                  │
 │    Binary semaphore: txDoneSem                         │
 │                                                        │
 │  Actuators:                                            │
 │    - Red LED    PTE31  (onboard, active-LOW)           │
 │    - Buzzer     PTA12  D3 (external, passive, PWM)     │
 │    - Servo SG90 PTA5   D5 (external, PWM)              │
 │                                                        │
 │  Sensor:                                               │
 │    - LDR (photoresistor) PTB2 A2 (ADC0_SE12)           │
 └────────────────────────────────────────────────────────┘
```

## Hardware

### Sensors (4 required)

| # | Sensor | Connected to | Interface | Polled / Interrupt | Purpose |
|---|--------|--------------|-----------|--------------------|---------|
| 1 | GY-521 MPU-6050 | ESP32-S2 | I2C (SDA/SCL) | Polled (~30 Hz) | Tilt → pitch & roll of airplane |
| 2 | Joystick module | ESP32-S2 | 2× ADC + 1 GPIO | Polled (ADC), interrupt (button) | Throttle (Y-axis), yaw (X-axis) |
| 3 | Push button | ESP32-S2 | GPIO | Interrupt (falling edge) | Fire / boost |
| 4 | LDR (photoresistor) | MCXC444 | ADC0_SE12, PTB2 (A2) | Polled (ldrTask, 100 ms) | Stealth mode — cover to dim lights |

### Actuators (3 required)

| # | Actuator | Connected to | Pin | Type | Purpose |
|---|----------|--------------|-----|------|---------|
| 1 | Red LED | MCXC444 | PTE31 (onboard) | GPIO, active-LOW | Game mode / hit / link-lost indicator |
| 2 | Passive buzzer | MCXC444 | PTA12, header D3 | TPM1_CH0 PWM (tone) | A tune per game event |
| 3 | Servo SG90 | MCXC444 | PTA5, header D5 | TPM0_CH2 PWM, 50 Hz | Lives gauge (0 lives = 0°, 3 = 180°) |

### MCXC444 wiring

| Signal | MCX pin | Header | Connect to |
|---|---|---|---|
| UART2 TX | PTE22 | — | ESP32 Serial1 RX |
| UART2 RX | PTE23 | — | ESP32 Serial1 TX |
| GND | GND | — | ESP32 GND (common ground is required) |
| Servo signal | PTA5 | D5 | SG90 orange wire (power the servo from 5 V, GND shared) |
| Buzzer signal | PTA12 | D3 | buzzer S pin (VCC → 3.3 V, GND → GND) |
| LDR | PTB2 | A2 | 3.3 V — LDR — A2 — 10 kΩ — GND |

The pins were chosen from the MCXC444 reference manual's pin-mux table and the FRDM schematic (`mcx/extras/`). PTD0/PTD4/PTE20/PTE21 are shared with the onboard LCD, and PTB0/PTB1 go to the MCU-Link I2C bus, so all of those are avoided.

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

- **Every frame is acknowledged.** The ESP32 can resend on `NAK`, or when no `ACK` arrives within ~200 ms.
- **Link watchdog:** if the MCX gets no valid frame for 2 s, it marks the link as lost and the red LED slow-blinks. The link recovers automatically on the next valid frame.

### What the MCXC444 does with it

| Input | Red LED | Buzzer | Servo |
|---|---|---|---|
| mode `IDLE` | short blip every 2 s | — | follows lives |
| mode `PLAY` | solid on | — | follows lives |
| mode `OVER` | 5 Hz blink | — | 0° |
| `EV HIT` | triple flash | falling two-note tune | — |
| `EV BONUS` | double flash | rising two-note tune | — |
| `EV START` / `EV OVER` | mode changes immediately | start / game-over tune | — |
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
| `buzzerTask` | 1 | MINIMAL+40 | Plays a tune per event from `buzzerQueue`. The tone comes from TPM1 hardware PWM, so pitch doesn't depend on task timing |
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
- Wire joystick (2× ADC + button GPIO interrupt).
- Wire push button with debounce.
- Package sensor readings into JSON for WebSocket.
- UART TX to MCXC444 with game events from relay.

### Stream 2 — ESP32 WiFi + WebSocket Client
- Connect to phone hotspot via WiFi.h.
- Establish WebSocket client to cloud relay (use ArduinoWebsockets library).
- Send sensor JSON at ~30 Hz.
- Receive game event JSON, parse, forward over UART.
- Handle reconnection on WiFi drop.

### Stream 3 — MCXC444 FreeRTOS Firmware
- ✅ UART RX and TX, both interrupt-driven, using the checksummed frame protocol (`$GS`/`$EV` in, `$ACK`/`$NAK`/`$LDR` out).
- ✅ LDR polling task with an interrupt-driven ADC, smoothing and hysteresis.
- ✅ LED, buzzer (TPM1 tone) and servo (TPM0 lives gauge) tasks.
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
- Map incoming sensor data to airplane controls (pitch/roll/throttle/fire).
- Send game events back (HIT, SCORE, BOOST).
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
│   └── flight_controller/
│       └── flight_controller.ino  ← Arduino sketch
├── relay/
│   ├── server.js              ← WebSocket relay
│   └── package.json
├── game/
│   ├── index.html             ← Three.js flight game
│   ├── style.css
│   └── game.js
└── docs/
    ├── interim_report.pdf
    └── final_report.pdf
```
