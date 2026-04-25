#pragma once

// Copy this file to `src/secrets.h` and fill in real values.
// `src/secrets.h` is gitignored — never commit it.
//
// Networking and STT credentials are loaded by `src/net.cpp` and
// `src/voice_stt.cpp` via `__has_include("secrets.h")`. If the file is
// missing or a value is left empty, those modules log a warning and
// skip work — the rest of the firmware (BLE Hardware Buddy + HID
// dictation toggle) continues to function.

#define WIFI_SSID         ""
#define WIFI_PSK          ""

// AWS API Gateway endpoint forwarding to the Transcribe Lambda.
// e.g. "https://abcd1234.execute-api.us-west-2.amazonaws.com/prod/transcribe"
#define STT_ENDPOINT_URL  ""

// API Gateway API key (sent as `x-api-key` header).
#define STT_API_KEY       ""
