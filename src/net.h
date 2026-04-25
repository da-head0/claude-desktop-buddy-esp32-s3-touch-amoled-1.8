#pragma once
#include <stddef.h>
#include <stdint.h>

// Brings up WiFi STA in the background using credentials from
// `src/secrets.h` (WIFI_SSID / WIFI_PSK). Non-blocking — connection
// status surfaces via netReady() and serial logs. Safe to call when
// secrets.h is missing or empty: WiFi is simply skipped.
void netInit();
bool netReady();

// Synchronous HTTPS POST. Body is opaque bytes (binary-safe). The
// response body is copied into outResp (NUL-terminated, truncated to
// outRespCap-1). Server certificates are validated against the
// ESP-IDF embedded x509 bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE),
// so any host signed by a major root CA — including AWS API Gateway
// via Amazon Trust Services — is trusted out of the box.
//
// Returns the response body length on HTTP 200, or a negative error
// code: -1 not connected, -2 client init, -3 open, -4 write,
// -(status) for non-200 HTTP status (e.g. -403 for 403 Forbidden).
int netHttpsPost(const char* url,
                 const uint8_t* body, size_t bodyLen,
                 const char* contentType,
                 const char* apiKey,
                 char* outResp, size_t outRespCap,
                 uint32_t timeoutMs);
