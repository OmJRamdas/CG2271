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
 │    communicationTask (pri 2) — UART RX/TX, parsing     │
 │    ledTask           (pri 1) — red LED control          │
 │    buzzerTask        (pri 1) — buzzer control           │
 │    sensorTask        (pri 1) — ADC polling (LDR)        │
 │    servoTask         (pri 1) — servo PWM control        │
 │                                                        │
 │  Sync:                                                 │
 │    Queues: uartRxQueue, ledQueue, buzzerQueue,         │
 │            servoQueue                                  │
 │    Mutex:  gameStateMutex (protects shared state)      │
 │                                                        │
 │  Actuators:                                            │
 │    - Red LED    PTE31  (onboard, active-LOW)           │
 │    - Buzzer     PTD4   (external, active-HIGH)         │
 │    - Servo SG90 PTD0   (external, PWM)                 │
 │                                                        │
 │  Sensor:                                               │
 │    - LDR (photoresistor) on ADC channel (e.g. PTB0)   │
 └────────────────────────────────────────────────────────┘
```

## Hardware

### Sensors (4 required)

| # | Sensor | Connected to | Interface | Polled / Interrupt | Purpose |
|---|--------|--------------|-----------|--------------------|---------|
| 1 | GY-521 MPU-6050 | ESP32-S2 | I2C (SDA/SCL) | Polled (~30 Hz) | Tilt → pitch & roll of airplane |
| 2 | Joystick module | ESP32-S2 | 2× ADC + 1 GPIO | Polled (ADC), interrupt (button) | Throttle (Y-axis), yaw (X-axis) |
| 3 | Push button | ESP32-S2 | GPIO | Interrupt (falling edge) | Fire / boost |
| 4 | LDR (photoresistor) | MCXC444 | ADC | Polled (sensorTask) | Stealth mode — cover to dim lights |

### Actuators (3 required)

| # | Actuator | Connected to | Pin | Type | Purpose |
|---|----------|--------------|-----|------|---------|
| 1 | Red LED | MCXC444 | PTE31 (onboard) | GPIO, active-LOW | Damage / warning indicator |
| 2 | Active buzzer | MCXC444 | PTD4 (external) | GPIO, active-HIGH | Collision / fire sound |
| 3 | Servo SG90 | MCXC444 | PTD0 (external) | PWM | Physical "wing flap" / tilt indicator |

## UART Protocol (Bi-directional, 9600 baud)

### ESP32 → MCXC444

```
GAME:<event>,<value>\n
```

| Message | Meaning |
|---------|---------|
| `GAME:HIT,1` | Player was hit — flash red LED + buzzer beep |
| `GAME:HIT,0` | Damage cleared |
| `GAME:BOOST,1` | Boost active — buzzer tone |
| `GAME:BOOST,0` | Boost ended |
| `GAME:ROLL,<deg>` | Current roll angle — servo mirrors it |
| `GAME:SCORE,<n>` | Score update — LED pattern |

### MCXC444 → ESP32

```
SENSOR:LDR,<raw_adc>\n
ACK:<event>\n
```

| Message | Meaning |
|---------|---------|
| `SENSOR:LDR,<value>` | LDR reading for stealth mode logic |
| `ACK:HIT` | Acknowledge received event |

## FreeRTOS Design

### Tasks

| Task | Priority | Stack | Role |
|------|----------|-------|------|
| `communicationTask` | 2 | MINIMAL+200 | UART RX parsing, TX responses, updates shared game state via mutex |
| `ledTask` | 1 | MINIMAL+100 | Reads ledQueue, drives red LED with patterns (flash, solid, off) |
| `buzzerTask` | 1 | MINIMAL+100 | Reads buzzerQueue, drives buzzer (beep patterns) |
| `sensorTask` | 1 | MINIMAL+100 | Polls LDR via ADC every 200 ms, sends reading over UART TX |
| `servoTask` | 1 | MINIMAL+100 | Reads servoQueue, sets servo PWM angle |

### Synchronization

- **Queues** (length 1, overwrite): `ledQueue`, `buzzerQueue`, `servoQueue` — latest command wins.
- **Queue** (length 5): `uartRxQueue` — buffered incoming UART messages.
- **Mutex**: `gameStateMutex` — protects a shared `GameState` struct written by `communicationTask`, read by actuator tasks for coordinated behavior.

### RTOS Demonstration

- **Time-slicing**: `ledTask`, `buzzerTask`, `sensorTask`, `servoTask` all at priority 1 — round-robin.
- **Pre-emption**: `communicationTask` at priority 2 pre-empts all priority-1 tasks on UART data.

### ISR

- `UART2_FLEXIO_IRQHandler`: character-by-character UART RX, enqueues complete messages. Non-blocking, uses `xQueueSendFromISR`. Set to lowest NVIC priority.

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
| Polling + interrupt | ✅ | ADC polling (LDR, joystick); interrupt (UART, button) |
| ≥1 ISR | ✅ | UART2 ISR |
| ≥3 tasks | ✅ | 5 tasks |
| ≥1 mutex/semaphore | ✅ | gameStateMutex |
| ≥1 queue | ✅ | 4+ queues |
| Time-slicing | ✅ | 4 tasks at priority 1 |
| Pre-emption | ✅ | communicationTask at priority 2 |
| Bi-directional UART | ✅ | ESP32→MCX (game events), MCX→ESP32 (LDR + ACK) |
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
- UART RX (already done) + add UART TX function.
- Parse `GAME:` protocol messages.
- Add `sensorTask` for LDR ADC polling.
- Add `servoTask` for PWM servo control.
- Add `gameStateMutex` for shared state protection.
- Transmit `SENSOR:LDR,<val>` back to ESP32.

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
├── mcxc444/
│   └── source/
│       └── main.c             ← FreeRTOS firmware
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
