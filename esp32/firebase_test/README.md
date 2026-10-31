# First Firebase test

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
