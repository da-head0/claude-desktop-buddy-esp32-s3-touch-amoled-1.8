#pragma once
#include <stddef.h>
#include <stdint.h>

// Push-to-talk voice capture → STT (AWS Transcribe via Lambda) → text.
//
// Lifecycle: Init (once) → Begin → Pump (every loop) → End (blocks 1–3s
// for the HTTPS round-trip). Result text is then available via Result()
// for the caller to type out via BLE HID.
//
// Audio format: 16 kHz mono 16-bit PCM, raw bytes posted to the
// configured endpoint. Lambda decodes with media_encoding="pcm".
// The PSRAM capture buffer is 960 KB (30 seconds maximum hold).

// Allocate the PSRAM capture buffer. Idempotent. Returns false on
// PSRAM allocation failure.
bool voiceSttInit();

// Begin capture. Resets the write pointer and starts the I2S RX
// pipeline. Returns false if Init failed or the mic refuses to start.
bool voiceSttBegin();

// Drain whatever samples are queued in the I2S DMA into the capture
// buffer. Call from the main loop while a capture is active. Returns
// true while still capturing; returns false the moment the buffer
// fills or the 30-second cap is hit (caller should then call End()).
bool voiceSttPump();

// True between Begin and End.
bool voiceSttCapturing();

// True between End() (POST start) and the moment a result is parsed.
// Useful for a ⏳ HUD indicator while the network round-trip is in
// flight; End() is synchronous so this is only true within that call.
bool voiceSttTranscribing();

// Stop capture, POST the buffer, parse the JSON response. Synchronous
// — blocks for the network round-trip (~1–3 seconds typical).
//
// Returns the number of UTF-8 bytes in the transcript on success
// (0 if Lambda returned empty text), or a negative error:
//   -1 not capturing / Init never succeeded
//   -2 STT_ENDPOINT_URL empty (set src/secrets.h)
//   -3 capture too short (< 1s) — skipped to save a round-trip
//   -4 JSON parse error
//   any other negative value: passthrough from netHttpsPost (HTTP
//   error code as -status, or -1..-4 transport errors).
int voiceSttEnd();

// Last transcript (NUL-terminated, UTF-8). Empty string if no
// capture has succeeded yet or the last capture was empty / errored.
const char* voiceSttResult();
