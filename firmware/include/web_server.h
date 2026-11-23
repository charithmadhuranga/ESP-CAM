// web_server.h — esp_http_server core + the API response envelope (plan 02
// task 1.2; SPEC section 6, API contract skill).
//
// ONE server instance for the whole device (R1 pattern: 7 sockets total,
// R1 used HTTP_CLIENTS + MAX_STREAMS = 7; we start smaller on the original
// ESP32: 4 API clients + 2 stream slots = 6 max_open_sockets). Each
// long-running response (stream, playback, download) owns one stream slot
// until it returns — a hard limit enforced by a bounded slot pool.
//
// Every JSON response uses the envelope:
//   { "ok": bool, "data": object|null, "error": {code,message}|null,
//     "request_id": "req-000001" }
// request_id is monotonic per boot (SPEC 6.0, API contract).
//
// Auth: every management/media/snapshot/stream/download route requires a
// valid session cookie (SPEC 4.2). State-changing cookie-authenticated
// requests additionally require a CSRF token (Origin/Host validated).
// Secrets are never returned: GET responses use password_configured-style
// booleans instead (AGENTS rule 17).
//
// Media/snapshot/stream routes take opaque IDs (med-…, rec-…, evt-…),
// never filesystem paths (SPEC 6.0, AGENTS rule 15).

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_http_server.h"

#include "auth.h"   // session/CSRF contract (task 1.2 auth)
#include "frame_manager.h"
#include "tasks.h"  // VC_STACK_HTTPD — single source of truth for stacks

namespace visioncam {

// --- server configuration --------------------------------------------------

// Total concurrent sockets the server may hold. LWIP_MAX_SOCKETS is raised
// to 16 in sdkconfig.defaults, so the IDF limit is 13 (16 − 3 internal).
// Browsers open 6–8 concurrent connections to the same origin; plus the live
// stream takes one slot; plus polling/API calls need headroom. 10 client
// sockets gives comfortable headroom without exhausting the limit.
constexpr int kHttpMaxOpenSockets = 10;

// The server task stack. Not a number of its own: tasks.h is the single
// source of truth for every task's stack (firmware/AGENTS.md), so this can
// never drift from VC_STACK_HTTPD. Measured HWM recorded in
// docs/ARCHITECTURE.md §4.
constexpr int kHttpServerStack = VC_STACK_HTTPD;

// LRU purge is always on (R1): evicts the least-recently-used cached
// static asset when flash is under pressure.
constexpr bool kHttpLruPurge = true;

// --- envelope helpers ------------------------------------------------------

// Standardised error codes (API contract skill).
enum class ErrCode : uint16_t {
    Ok = 0,
    Unauthenticated = 1,
    SessionExpired = 2,
    CsrfInvalid = 3,
    ValidationFailed = 4,
    NotFound = 5,
    SdNotMounted = 6,
    SdFull = 7,
    SdWriteFailed = 8,
    StreamSlotLimit = 9,
    PlaybackBusy = 10,
    TimeUnsynchronised = 11,
    RateLimited = 12,
    Internal = 13,
    // Additive 2026-11-21 (Phase 7): broker/backend unreachable. The
    // skill's standard set names it MQTT_UNAVAILABLE; numeric 14 -> 503.
    MqttUnavailable = 14,
    // Additive 2026-11-21 (Phase 8): network-storage backend unreachable
    // (test failed / upload queue full / heap gate). Numeric 15 -> 503.
    NetworkStorageUnavailable = 15,
    WifiUnavailable = 16,
};

// Writes an OK envelope: {ok:true, data:<payload>, error:null, request_id:N}.
// payload may be nullptr (data omitted). Content-Type application/json.
esp_err_t httpReplyOk(httpd_req_t* req, uint32_t request_id,
                      const char* payload = nullptr);

// Writes an error envelope: {ok:false, data:null, error:{code,message},
// request_id:N}. message is caller-supplied and must never contain secrets
// or stack traces.
esp_err_t httpReplyError(httpd_req_t* req, uint32_t request_id,
                         ErrCode code, const char* message);

// Sends raw bytes as a JPEG/PNG/etc. with the given content type and
// Content-Length. Used by /camera/stream parts and /snapshots/{id}/image.
esp_err_t httpReplyBytes(httpd_req_t* req, const char* content_type,
                         const uint8_t* data, size_t len,
                         const char* cache_control = "no-cache");

// Returns the next monotonic request_id (per boot). First is 1.
uint32_t httpNextRequestId();

// --- server lifecycle ------------------------------------------------------

// Creates and starts the HTTP server on port 80. Idempotent-safe.
esp_err_t webServerStart();

// Stops the server and frees its resources. Safe when already stopped.
esp_err_t webServerStop();

// True while the server is running.
bool webServerRunning();

// --- routes ----------------------------------------------------------------

// Registers every Phase 1 route (auth, device, camera, system). Call after
// webServerStart(). Auth/CSRF middleware is wired here (task 1.2 auth).
esp_err_t webServerRegisterRoutes();

// A stream slot is held for the lifetime of a long-running response.
// frameManagerAcquireFrame() etc. are called by the stream handler via
// frame leases (AGENTS rule 9 — the handler never calls fb_get itself).

}  // namespace visioncam