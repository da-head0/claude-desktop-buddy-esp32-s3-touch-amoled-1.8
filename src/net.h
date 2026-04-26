#pragma once
#include <stddef.h>
#include <stdint.h>

// Register WiFi event handlers and stash credentials. Does NOT touch
// the radio — call netStart() when you actually want WiFi up. Safe
// when secrets.h is missing or empty: every other net*() call becomes
// a silent no-op.
void netInit();

// Bring up STA mode and start associating with the configured SSID.
// Idempotent — safe to call repeatedly when already started or
// connecting. Async: connection completes in the background; check
// netReady() or call netWaitReady().
void netStart();

// Disconnect and fully power down the WiFi radio. Idempotent. Caller
// is expected to pair this with netStart() based on UI activity (e.g.
// keep WiFi on only while the screen is on).
void netStop();

// True iff currently associated and DHCP-good.
bool netReady();

// Block up to timeoutMs (in 50 ms slices) waiting for netReady().
// Auto-calls netStart() if the radio is currently off, so callers
// don't need to remember whether they started it. Returns true on
// connection, false on timeout.
bool netWaitReady(uint32_t timeoutMs);

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
