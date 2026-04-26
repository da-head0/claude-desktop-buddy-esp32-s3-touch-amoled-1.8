#include "voice_stt.h"
#include "hw/audio.h"
#include "net.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <string.h>

#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef STT_ENDPOINT_URL
  #define STT_ENDPOINT_URL ""
#endif
#ifndef STT_API_KEY
  #define STT_API_KEY ""
#endif

static constexpr size_t SAMPLE_RATE     = 16000;
static constexpr size_t MAX_DURATION_MS = 30000;
static constexpr size_t BUF_SAMPLES     = SAMPLE_RATE * MAX_DURATION_MS / 1000;
static constexpr size_t BUF_BYTES       = BUF_SAMPLES * sizeof(int16_t);

// ~64 ms worth of audio per pump call. Small enough that pumping never
// blocks the UI loop, large enough to keep i2s_read calls cheap.
static constexpr size_t PUMP_CHUNK      = 1024;

// Minimum capture before we bother POSTing — guards against a finger
// tap that briefly crosses the 1-second voice threshold.
static constexpr size_t MIN_SAMPLES     = SAMPLE_RATE;  // 1 second

static int16_t* s_buf            = nullptr;
static size_t   s_writeSamples   = 0;
static uint32_t s_startMs        = 0;
static bool     s_capturing      = false;
static bool     s_transcribing   = false;
static char     s_result[1024]   = {0};

bool voiceSttInit() {
  if (s_buf) return true;
  s_buf = (int16_t*)heap_caps_malloc(BUF_BYTES, MALLOC_CAP_SPIRAM);
  if (!s_buf) {
    Serial.printf("[stt] PSRAM alloc failed (%u KB requested)\n",
                  (unsigned)(BUF_BYTES / 1024));
    return false;
  }
  Serial.printf("[stt] PSRAM buffer ready (%u KB, %u s @ %u Hz)\n",
                (unsigned)(BUF_BYTES / 1024),
                (unsigned)(MAX_DURATION_MS / 1000),
                (unsigned)SAMPLE_RATE);
  return true;
}

bool voiceSttBegin() {
  if (!s_buf && !voiceSttInit()) return false;
  if (s_capturing) return true;  // already running
  if (!hwAudioMicStart()) {
    Serial.println("[stt] hwAudioMicStart failed");
    return false;
  }
  s_writeSamples = 0;
  s_startMs      = millis();
  s_capturing    = true;
  s_result[0]    = '\0';
  Serial.printf("[stt] mic start (%u Hz mono)\n", (unsigned)SAMPLE_RATE);
  return true;
}

bool voiceSttPump() {
  if (!s_capturing) return false;

  // Drain whatever is queued without blocking. Multiple chunks per
  // pump in case the loop fell behind.
  while (s_writeSamples < BUF_SAMPLES) {
    size_t want = BUF_SAMPLES - s_writeSamples;
    if (want > PUMP_CHUNK) want = PUMP_CHUNK;
    int got = hwAudioMicRead(s_buf + s_writeSamples, want, 0);
    if (got <= 0) break;
    s_writeSamples += (size_t)got;
  }

  if (s_writeSamples >= BUF_SAMPLES) {
    Serial.println("[stt] capture buffer full, stopping");
    return false;
  }
  if ((millis() - s_startMs) > MAX_DURATION_MS) {
    Serial.println("[stt] capture duration cap, stopping");
    return false;
  }
  return true;
}

bool voiceSttCapturing()    { return s_capturing; }
bool voiceSttTranscribing() { return s_transcribing; }
const char* voiceSttResult() { return s_result; }

int voiceSttEnd() {
  if (!s_capturing) return -1;
  s_capturing = false;
  hwAudioMicStop();

  size_t samples = s_writeSamples;
  size_t bytes   = samples * sizeof(int16_t);
  Serial.printf("[stt] mic stop, n=%u samples (%u bytes, %u ms)\n",
                (unsigned)samples, (unsigned)bytes,
                (unsigned)(samples * 1000 / SAMPLE_RATE));

  if (!STT_ENDPOINT_URL[0]) {
    Serial.println("[stt] STT_ENDPOINT_URL empty — set src/secrets.h");
    return -2;
  }
  if (samples < MIN_SAMPLES) {
    Serial.println("[stt] capture <1s, skipping POST");
    return -3;
  }

  // WiFi is brought up at wake() and torn down at screen-off, so it's
  // usually associated by the time we get here. Cold path: user PTTs
  // immediately after wake → STA hasn't finished DHCP → block briefly.
  // Audio is already fully captured into PSRAM; only the upload waits.
  uint32_t waitT0 = millis();
  if (!netWaitReady(8000)) {
    Serial.printf("[stt] WiFi not ready after %ums, abort POST\n",
                  (unsigned)(millis() - waitT0));
    return -1;
  }
  uint32_t waitMs = millis() - waitT0;
  if (waitMs > 100) {
    Serial.printf("[stt] WiFi ready after %ums\n", (unsigned)waitMs);
  }

  static char respBuf[2048];
  s_transcribing = true;
  int rc = netHttpsPost(STT_ENDPOINT_URL,
                        (const uint8_t*)s_buf, bytes,
                        "application/octet-stream",
                        STT_API_KEY,
                        respBuf, sizeof(respBuf),
                        15000);
  s_transcribing = false;
  if (rc < 0) {
    Serial.printf("[stt] POST failed rc=%d\n", rc);
    return rc;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, respBuf, (size_t)rc);
  if (err) {
    Serial.printf("[stt] JSON parse error: %s, body=%s\n",
                  err.c_str(), respBuf);
    return -4;
  }
  const char* text = doc["text"] | "";
  size_t len = strlen(text);
  if (len >= sizeof(s_result)) len = sizeof(s_result) - 1;
  memcpy(s_result, text, len);
  s_result[len] = '\0';
  Serial.printf("[stt] text=\"%s\" (%u bytes)\n", s_result, (unsigned)len);
  return (int)len;
}
