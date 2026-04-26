#include "net.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>

#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID ""
#endif
#ifndef WIFI_PSK
  #define WIFI_PSK ""
#endif

// Tracks whether the radio has been brought up via netStart(). Lets
// netStart()/netStop() be idempotent and lets netWaitReady() lazily
// auto-start when a caller (e.g. voiceSttEnd) skipped it.
static bool s_started = false;

void netInit() {
  if (!WIFI_SSID[0]) {
    Serial.println("[net] WIFI_SSID empty — every net* call will no-op (set src/secrets.h)");
    return;
  }
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        Serial.println("[net] wifi associated, waiting for DHCP");
        break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.printf("[net] wifi connected ip=%s rssi=%d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
        uint8_t r = info.wifi_sta_disconnected.reason;
        const char* hint = "";
        switch (r) {
          case WIFI_REASON_NO_AP_FOUND:
            hint = " — SSID not seen (check spelling, 2.4GHz only, range)";
            break;
          case WIFI_REASON_AUTH_FAIL:
          case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            hint = " — wrong password";
            break;
          case WIFI_REASON_HANDSHAKE_TIMEOUT:
            hint = " — handshake timeout (router slow / WPA mode mismatch)";
            break;
          case WIFI_REASON_ASSOC_EXPIRE:
          case WIFI_REASON_ASSOC_TOOMANY:
            hint = " — router refused (too many clients?)";
            break;
        }
        Serial.printf("[net] wifi disconnected reason=%u%s\n", r, hint);
        break;
      }
      default:
        break;
    }
  });
  // Keep credentials in RAM only; a deliberate netStop()/netStart()
  // cycle relies on the cached SSID/PSK rather than NVS reads.
  WiFi.persistent(false);
}

void netStart() {
  if (s_started) return;
  if (!WIFI_SSID[0]) return;
  WiFi.mode(WIFI_STA);
  // AutoReconnect is harmless once the radio is fully off (mode OFF
  // suspends the STA state machine), so we leave it on for the run
  // between netStart() and netStop().
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PSK);
  s_started = true;
  Serial.printf("[net] start (ssid=%s)\n", WIFI_SSID);
}

void netStop() {
  if (!s_started) return;
  // disconnect(false) tears down the association without erasing the
  // cached credentials; mode(WIFI_OFF) powers the radio down so the
  // big draw is gone until the next netStart().
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
  s_started = false;
  Serial.println("[net] stop");
}

bool netReady() {
  return WiFi.status() == WL_CONNECTED;
}

bool netWaitReady(uint32_t timeoutMs) {
  if (!s_started) netStart();   // lenient: caller may have forgotten
  uint32_t deadline = millis() + timeoutMs;
  while ((int32_t)(millis() - deadline) < 0) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(50);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Captured by the HTTP event handler so we can pull the response body out
// of esp_http_client_perform() without dealing with chunked-vs-content-
// length manually. perform() is more robust than open/write/read_response
// for large request bodies — the latter occasionally drops the response
// headers when the POST body is tens of KB on slow uplinks (observed at
// 124 KB / 16 kHz × 4 s captures over 2.4 GHz WiFi).
struct HttpRespCtx {
  char*  buf;
  size_t cap;
  size_t len;
};

static esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  auto* ctx = (HttpRespCtx*)evt->user_data;
  if (!ctx || !ctx->buf || ctx->cap == 0) return ESP_OK;
  size_t avail = (ctx->cap > ctx->len + 1) ? (ctx->cap - ctx->len - 1) : 0;
  size_t copy = ((size_t)evt->data_len < avail) ? (size_t)evt->data_len : avail;
  if (copy > 0) {
    memcpy(ctx->buf + ctx->len, evt->data, copy);
    ctx->len += copy;
    ctx->buf[ctx->len] = '\0';
  }
  return ESP_OK;
}

int netHttpsPost(const char* url,
                 const uint8_t* body, size_t bodyLen,
                 const char* contentType,
                 const char* apiKey,
                 char* outResp, size_t outRespCap,
                 uint32_t timeoutMs) {
  if (!netReady()) {
    Serial.println("[net] POST: WiFi not connected");
    return -1;
  }

  HttpRespCtx ctx = { outResp, outRespCap, 0 };
  if (outResp && outRespCap > 0) outResp[0] = '\0';

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = (int)timeoutMs;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
  cfg.event_handler = httpEventHandler;
  cfg.user_data = &ctx;
  cfg.buffer_size = 1536;       // response read buffer
  cfg.buffer_size_tx = 2048;    // request headers + chunked write staging

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    Serial.println("[net] POST: client init failed");
    return -2;
  }

  if (contentType && contentType[0]) {
    esp_http_client_set_header(client, "Content-Type", contentType);
  }
  if (apiKey && apiKey[0]) {
    esp_http_client_set_header(client, "x-api-key", apiKey);
  }
  esp_http_client_set_post_field(client, (const char*)body, (int)bodyLen);

  esp_err_t err = esp_http_client_perform(client);
  int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
  esp_http_client_cleanup(client);

  if (err != ESP_OK) {
    Serial.printf("[net] POST: perform failed: %s\n", esp_err_to_name(err));
    return -3;
  }

  Serial.printf("[net] POST → %d, body %u bytes\n", status, (unsigned)ctx.len);
  return (status == 200) ? (int)ctx.len : -status;
}
