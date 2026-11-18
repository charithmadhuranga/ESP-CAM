// web_server.cpp — see web_server.h.
//
// esp_http_server core (SPEC 6.0): one instance, lru_purge on,
// max_open_sockets = 6 (4 API clients + 2 stream slots — smaller than R1's
// 7), 4 KB task stack. Every JSON response uses the {ok,data,error,
// request_id} envelope with a monotonic per-boot request_id.
//
// Auth middleware (SPEC 4.2): every management/media/snapshot/stream route
// requires a valid session cookie; state-changing cookie-authenticated
// requests additionally require a CSRF token (X-CSRF-Token header matching
// the csrf cookie). No credentials or tokens in query strings. Secrets are
// never returned: GET responses use password_configured-style booleans.
//
// Routes are registered once at boot; later phases add recording/snapshot/
// storage/timelapse/AI/MQTT routes to this same table.

#include "web_server.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_http_server.h"
#include "esp_log.h"

#include "auth.h"
#include "camera_manager.h"
#include "camera_recovery.h"
#include "network_manager.h"
#include "resource_monitor.h"
#include "ring_log.h"
#include "storage_manager.h"
#include "web_assets_manifest.h"

namespace visioncam {

const char* TAG = "vc_web";

namespace {

// --- auth middleware --------------------------------------------------------
constexpr char kSessionCookie[] = "visioncam_session";
constexpr char kCsrfCookie[] = "visioncam_csrf";

// Reads the session cookie value, base64-decodes into token_out.
esp_err_t readSessionCookie(httpd_req_t* req, uint8_t* token_out,
                            size_t token_cap, size_t& token_len) {
    char buf[512] = {};
    size_t buf_size = sizeof(buf);
    if (httpd_req_get_hdr_value_str(req, "Cookie", buf, buf_size) != ESP_OK)
        return ESP_ERR_NOT_FOUND;
    const char* p = std::strstr(buf, kSessionCookie);
    if (p == nullptr) return ESP_ERR_NOT_FOUND;
    p += std::strlen(kSessionCookie);
    if (*p != '=') return ESP_ERR_NOT_FOUND;
    ++p;
    const char* end = std::strchr(p, ';');
    size_t cookie_len =
        (end == nullptr) ? std::strlen(p) : static_cast<size_t>(end - p);
    if (cookie_len == 0 || cookie_len > token_cap * 2) return ESP_ERR_INVALID_SIZE;
    char b64[64] = {};
    if (cookie_len >= sizeof(b64)) return ESP_ERR_INVALID_SIZE;
    std::memcpy(b64, p, cookie_len);
    size_t dec = vc_b64d(b64, cookie_len, token_out, token_cap);
    if (dec == 0 || dec > kCsrfTokenBytes) return ESP_ERR_INVALID_SIZE;
    token_len = dec;
    return ESP_OK;
}

// Requires an authenticated session cookie. Returns ESP_OK when authenticated.
esp_err_t requireAuth(httpd_req_t* req) {
    uint8_t token[kCsrfTokenBytes] = {};
    size_t token_len = 0;
    if (readSessionCookie(req, token, sizeof(token), token_len) != ESP_OK) {
        return ESP_FAIL;
    }
    SessionView view{};
    if (authValidateSession(token, token_len, &view) != ESP_OK) return ESP_FAIL;
    return ESP_OK;
}

// CSRF double-submit: X-CSRF-Token header vs csrf cookie.
esp_err_t requireCsrf(httpd_req_t* req) {
    char header[128] = {};
    size_t header_size = sizeof(header);
    if (httpd_req_get_hdr_value_str(req, "X-CSRF-Token", header,
                                    header_size) != ESP_OK)
        return ESP_FAIL;
    char cookie[512] = {};
    size_t cookie_size = sizeof(cookie);
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie,
                                    cookie_size) != ESP_OK)
        return ESP_FAIL;
    const char* p = std::strstr(cookie, kCsrfCookie);
    if (p == nullptr) return ESP_FAIL;
    p += std::strlen(kCsrfCookie);
    if (*p != '=') return ESP_FAIL;
    ++p;
    const char* end = std::strchr(p, ';');
    size_t cookie_len =
        (end == nullptr) ? std::strlen(p) : static_cast<size_t>(end - p);
    if (cookie_len == 0 || cookie_len > 128) return ESP_FAIL;
    char cookie_val[128] = {};
    std::memcpy(cookie_val, p, cookie_len);
    return authCsrfValidate(header, cookie_val);
}

// Reads a JSON body into a bounded stack buffer.
esp_err_t readJsonBody(httpd_req_t* req, char* out, size_t out_cap,
                       size_t& out_len) {
    size_t remaining = req->content_len;
    out_len = 0;
    while (remaining > 0) {
        size_t to_read = std::min(remaining, out_cap - out_len - 1);
        if (to_read == 0) return ESP_ERR_NO_MEM;
        int recv = httpd_req_recv(req, out + out_len, to_read);
        if (recv <= 0) return ESP_FAIL;
        out_len += static_cast<size_t>(recv);
        remaining -= static_cast<size_t>(recv);
    }
    out[out_len] = '\0';
    return ESP_OK;
}

// Extracts a JSON scalar value for a key from a small request body.
// Accepts a quoted string ("VGA"), a bare number (12) or a bare literal
// (true) — the webui sends framesize/quality/max_used_percent as JSON
// numbers, and the old quoted-only parser silently failed on them, which
// made a PUT report success while writing nothing (observed on hardware:
// retention max_used_percent stayed at its default after a "successful"
// update). Nested objects/arrays are not supported (returns false) — no
// endpoint takes them.
bool jsonScalar(const char* body, const char* key, char* out, size_t out_cap) {
    const char* k = std::strstr(body, key);
    if (k == nullptr) return false;
    k = std::strchr(k, ':');
    if (k == nullptr) return false;
    ++k;
    while (*k == ' ') ++k;
    if (*k == '{' || *k == '[') return false;   // nested value: unsupported
    if (*k == '"') {
        ++k;
        const char* end = std::strchr(k, '"');
        if (end == nullptr) return false;
        size_t n = static_cast<size_t>(end - k);
        if (n >= out_cap) return false;
        std::memcpy(out, k, n);
        out[n] = '\0';
        return true;
    }
    // Bare scalar: copy until a JSON structural/whitespace delimiter.
    size_t n = 0;
    while (*k != '\0' && *k != ',' && *k != '}' && *k != ']' &&
           *k != ' ' && *k != '\n' && *k != '\r' && *k != '\t') {
        if (n + 1 >= out_cap) return false;
        out[n++] = *k++;
    }
    if (n == 0) return false;
    out[n] = '\0';
    return true;
}

// Strict unsigned parse: the WHOLE string must be decimal digits (rejects
// "", "12abc", "-3", overflows). Returns false on any garbage so a typo
// can never be silently coerced to 0 (which is a valid policy value —
// "0 disables size retention" — and would have looked like success).
bool parseU32Strict(const char* s, uint32_t& out) {
    if (s == nullptr || *s == '\0') return false;
    uint64_t v = 0;
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10u + static_cast<uint64_t>(*p - '0');
        if (v > 0xFFFFFFFFull) return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

// Shared retention payload builder — GET and PUT answers use the SAME
// shape so consumers never branch on the method (schema_version included:
// the policy is a persisted schema, SPEC 7).
void retentionJson(const RetentionPolicy& rp, char* out, size_t cap) {
    std::snprintf(out, cap,
                  "{\"retention_days_manual\":%u,"
                  "\"retention_days_motion\":%u,"
                  "\"retention_days_detection\":%u,"
                  "\"retention_days_timelapse\":%u,"
                  "\"max_used_percent\":%u,"
                  "\"enabled\":%s,\"schema_version\":1}",
                  static_cast<unsigned int>(rp.retention_days_manual),
                  static_cast<unsigned int>(rp.retention_days_motion),
                  static_cast<unsigned int>(rp.retention_days_detection),
                  static_cast<unsigned int>(rp.retention_days_timelapse),
                  static_cast<unsigned int>(rp.max_used_percent),
                  rp.enabled ? "true" : "false");
}

// Sends a 401 with the JSON error envelope (auth failure path).
esp_err_t replyUnauthenticated(httpd_req_t* req, uint32_t rid) {
    return httpd_resp_set_status(req, "401 Unauthorized") == ESP_OK
               ? httpReplyError(req, rid, ErrCode::Unauthenticated,
                                "no valid session")
               : ESP_FAIL;
}

// --- static asset server -----------------------------------------------------
// Looks up the request URI in the embedded asset manifest and serves the
// gzip-compressed bytes with Content-Encoding: gzip. Unknown URIs fall
// through to index.html (SPA fallback for client-side routing).
static esp_err_t assetHandler(httpd_req_t* req) {
    const char* uri = req->uri;
    for (size_t i = 0; i < web_assets_count; ++i) {
        if (std::strcmp(uri, web_assets[i].url) == 0) {
            httpd_resp_set_type(req, web_assets[i].mime);
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
            httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");
            char len_buf[16];
            std::snprintf(len_buf, sizeof(len_buf), "%u",
                          static_cast<unsigned>(web_assets[i].comp_size));
            httpd_resp_set_hdr(req, "Content-Length", len_buf);
            return httpd_resp_send(req,
                                   reinterpret_cast<const char*>(
                                       web_assets[i].data),
                                   web_assets[i].comp_size);
        }
    }
    // Not found — serve index.html as SPA fallback (client-side routing).
    for (size_t i = 0; i < web_assets_count; ++i) {
        if (std::strcmp("/", web_assets[i].url) == 0) {
            httpd_resp_set_type(req, "text/html");
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
            char len_buf[16];
            std::snprintf(len_buf, sizeof(len_buf), "%u",
                          static_cast<unsigned>(web_assets[i].comp_size));
            httpd_resp_set_hdr(req, "Content-Length", len_buf);
            return httpd_resp_send(req,
                                   reinterpret_cast<const char*>(
                                       web_assets[i].data),
                                   web_assets[i].comp_size);
        }
    }
    return httpd_resp_set_status(req, "404 Not Found") == ESP_OK
               ? httpd_resp_send(req, "Not found", 9)
               : ESP_FAIL;
}

// Monotonic per-boot request_id (SPEC 6.0).
std::atomic<uint32_t> s_request_id{0};

// Sends the full JSON envelope. payload is inserted verbatim as the "data"
// field (must be valid JSON or "null").
esp_err_t sendEnvelope(httpd_req_t* req, bool ok, uint32_t request_id,
                       ErrCode code, const char* message,
                       const char* payload) {
    char buf[512] = {};
    int n = 0;
    n += std::snprintf(buf + n, sizeof(buf) - n,
                       "{\"ok\":%s,\"request_id\":%u",
                       ok ? "true" : "false",
                       static_cast<unsigned int>(request_id));
    if (code != ErrCode::Ok && message != nullptr) {
        n += std::snprintf(buf + n, sizeof(buf) - n,
                           ",\"error\":{\"code\":%u,\"message\":\"%s\"}",
                           static_cast<unsigned>(code), message);
    } else {
        n += std::snprintf(buf + n, sizeof(buf) - n, ",\"error\":null");
    }
    if (payload != nullptr) {
        n += std::snprintf(buf + n, sizeof(buf) - n, ",\"data\":%s", payload);
    } else {
        n += std::snprintf(buf + n, sizeof(buf) - n, ",\"data\":null");
    }
    n += std::snprintf(buf + n, sizeof(buf) - n, "}");
    if (n < 0 || static_cast<size_t>(n) >= sizeof(buf)) n = sizeof(buf) - 1;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

}  // namespace

// Envelope helpers declared in web_server.h (defined at visioncam scope so
// they match the header declarations; they call the anonymous-namespace
// sendEnvelope() and access s_request_id).
esp_err_t httpReplyOk(httpd_req_t* req, uint32_t request_id,
                      const char* payload) {
    return sendEnvelope(req, true, request_id, ErrCode::Ok, nullptr, payload);
}

esp_err_t httpReplyError(httpd_req_t* req, uint32_t request_id, ErrCode code,
                         const char* message) {
    // Appropriate HTTP status per the API contract (the envelope still
    // carries the numeric ErrCode). String literals only: httpd stores the
    // status POINTER (httpd_txrx.c:208-224), no copy.
    // Contract change logged in CHANGELOG (rule 27/28): error envelopes
    // used to arrive with HTTP 200; the webui client parses the envelope
    // regardless of status (webui/src/lib/api.ts parseEnvelope), so its
    // behaviour is unchanged. 401 paths were already explicit.
    const char* status = nullptr;
    switch (code) {
    case ErrCode::Ok:                 break;
    case ErrCode::Unauthenticated:
    case ErrCode::SessionExpired:     status = "401 Unauthorized"; break;
    case ErrCode::CsrfInvalid:        status = "403 Forbidden"; break;
    case ErrCode::ValidationFailed:   status = "400 Bad Request"; break;
    case ErrCode::NotFound:           status = "404 Not Found"; break;
    case ErrCode::RateLimited:        status = "429 Too Many Requests"; break;
    case ErrCode::SdNotMounted:
    case ErrCode::StreamSlotLimit:
    case ErrCode::PlaybackBusy:
    case ErrCode::TimeUnsynchronised: status = "503 Service Unavailable"; break;
    case ErrCode::SdFull:             status = "507 Insufficient Storage"; break;
    case ErrCode::SdWriteFailed:
    case ErrCode::Internal:           status = "500 Internal Server Error"; break;
    }
    if (status != nullptr) httpd_resp_set_status(req, status);
    return sendEnvelope(req, false, request_id, code, message, nullptr);
}

esp_err_t httpReplyBytes(httpd_req_t* req, const char* content_type,
                         const uint8_t* data, size_t len,
                         const char* cache_control) {
    httpd_resp_set_type(req, content_type);
    if (cache_control != nullptr)
        httpd_resp_set_hdr(req, "Cache-Control", cache_control);
    char len_buf[16];
    std::snprintf(len_buf, sizeof(len_buf), "%u",
                  static_cast<unsigned>(len));
    httpd_resp_set_hdr(req, "Content-Length", len_buf);
    return httpd_resp_send(req, reinterpret_cast<const char*>(data), len);
}

uint32_t httpNextRequestId() { return s_request_id.fetch_add(1) + 1; }

// ===========================================================================
// routes (envelope helpers httpReplyOk/Error/Bytes and httpNextRequestId are
// declared in web_server.h)
// ===========================================================================

// --- GET /auth/session -------------------------------------------------------
static esp_err_t authSessionHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    uint8_t token[kCsrfTokenBytes] = {};
    size_t tl = 0;
    SessionView view{};
    if (readSessionCookie(req, token, sizeof(token), tl) == ESP_OK &&
        authValidateSession(token, tl, &view) == ESP_OK) {
        char payload[220] = {};
        std::snprintf(payload, sizeof(payload),
                      "{\"authenticated\":true,\"username\":\"admin\","
                      "\"password_change_required\":%s}",
                      view.password_change_required ? "true" : "false");
        return httpReplyOk(req, rid, payload);
    }
    return replyUnauthenticated(req, rid);
}

// --- POST /auth/login --------------------------------------------------------
static esp_err_t authLoginHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    char body[256] = {};
    size_t blen = 0;
    if (readJsonBody(req, body, sizeof(body), blen) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "invalid JSON body");
    }
    char user[64] = {};
    char pass[64] = {};
    if (!jsonScalar(body, "\"username\"", user, sizeof(user)) ||
        !jsonScalar(body, "\"password\"", pass, sizeof(pass)) ||
        user[0] == '\0' || pass[0] == '\0') {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "username and password required");
    }
    if (authRecordFailedAttempt(user) == AUTH_ERR_RATE_LIMITED) {
        return httpReplyError(req, rid, ErrCode::RateLimited,
                              "too many login attempts");
    }
    if (!authVerifyPassword(user, pass)) {
        return httpReplyError(req, rid, ErrCode::Unauthenticated,
                              "invalid username or password");
    }
    authClearFailedAttempts(user);
    uint8_t token[kCsrfTokenBytes] = {};
    size_t tlen = 0;
    SessionId sid = 0;
    bool change_required = true;
    if (authCreateSession(user, pass, token, sizeof(token), tlen, &sid,
                          &change_required) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "session creation failed");
    }
    char csrf_b64[64] = {};
    size_t cslen = 0;
    authCsrfGenerate(csrf_b64, sizeof(csrf_b64), cslen);
    char token_b64[64] = {};
    vc_b64e(static_cast<const uint8_t*>(token), tlen, token_b64,
            sizeof(token_b64));
    // Combine both cookies into one Set-Cookie header because
    // httpd_resp_set_hdr() replaces rather than appends.
    char set_cookie[512] = {};
    std::snprintf(set_cookie, sizeof(set_cookie),
                  "%s=%s; Path=/; HttpOnly; SameSite=Lax, %s=%s; Path=/; "
                  "HttpOnly; SameSite=Lax",
                  kSessionCookie, token_b64, kCsrfCookie, csrf_b64);
    httpd_resp_set_hdr(req, "Set-Cookie", set_cookie);
    return httpReplyOk(req, rid,
                       change_required
                           ? "{\"password_change_required\":true}"
                           : "{\"password_change_required\":false}");
}

// --- POST /auth/logout -------------------------------------------------------
static esp_err_t authLogoutHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    (void)requireAuth(req);
    authRevokeAllSessions();
    httpd_resp_set_hdr(req, "Set-Cookie",
                       "visioncam_session=; Path=/; HttpOnly; SameSite=Lax; "
                       "Max-Age=0, visioncam_csrf=; Path=/; HttpOnly; "
                       "SameSite=Lax; Max-Age=0");
    return httpReplyOk(req, rid, "{\"ok\":true}");
}

// --- POST /auth/complete-first-login ----------------------------------------
static esp_err_t authCompleteFirstLoginHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireCsrf(req) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                              "CSRF token missing or invalid");
    }
    char body[256] = {};
    size_t blen = 0;
    if (readJsonBody(req, body, sizeof(body), blen) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "invalid JSON body");
    }
    char current[64] = {};
    char new_pass[64] = {};
    if (!jsonScalar(body, "\"current_password\"", current, sizeof(current)) ||
        !jsonScalar(body, "\"new_password\"", new_pass, sizeof(new_pass)) ||
        current[0] == '\0' || new_pass[0] == '\0') {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "current_password and new_password required");
    }
    if (authCompleteFirstLogin(current, new_pass) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Unauthenticated,
                              "current password incorrect");
    }
    return httpReplyOk(req, rid, "{\"password_change_required\":false}");
}

// --- POST /auth/change-password ---------------------------------------------
static esp_err_t authChangePasswordHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (requireCsrf(req) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                              "CSRF token missing or invalid");
    }
    char body[256] = {};
    size_t blen = 0;
    if (readJsonBody(req, body, sizeof(body), blen) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "invalid JSON body");
    }
    char current[64] = {};
    char new_user[64] = {};
    char new_pass[64] = {};
    if (!jsonScalar(body, "\"current_password\"", current, sizeof(current)) ||
        !jsonScalar(body, "\"new_password\"", new_pass, sizeof(new_pass)) ||
        current[0] == '\0' || new_pass[0] == '\0') {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "current_password and new_password required");
    }
    jsonScalar(body, "\"new_username\"", new_user, sizeof(new_user));
    if (authChangePassword(0, current,
                           new_user[0] != '\0' ? new_user : nullptr,
                           new_pass) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Unauthenticated,
                              "current password incorrect");
    }
    return httpReplyOk(req, rid, "{}");
}

// --- GET /device -------------------------------------------------------------
static esp_err_t deviceHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    char payload[300] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"device_id\":\"visioncam\","
                  "\"device_name\":\"VisionCam Pro\","
                  "\"firmware_version\":\"0.1.0-phase1\","
                  "\"schema_version\":1}");
    return httpReplyOk(req, rid, payload);
}

// --- GET /status -------------------------------------------------------------
static esp_err_t statusHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    const WifiStatus ws = networkManagerStatus();
    ResourceSnapshot rs{};
    resourceMonitorSnapshot(rs);
    char payload[420] = {};
    size_t n = 0;
    n += std::snprintf(payload + n, sizeof(payload) - n,
                       "{\"wifi\":{\"state\":\"%s\",\"configured\":%s,"
                       "\"radio_up\":%s,\"rssi\":%d,\"connects\":%u,"
                       "\"disconnects\":%u},\"camera\":{\"initialised\":%s,"
                       "\"framesize\":%d},\"time_synced\":%s}",
                       wifiStateName(ws.state),
                       ws.configured ? "true" : "false",
                       ws.radio_up ? "true" : "false", ws.rssi,
                       static_cast<unsigned int>(ws.connects),
                       static_cast<unsigned int>(ws.disconnects),
                       cameraManagerInitialised() ? "true" : "false",
                       static_cast<int>(cameraManagerFramesize()),
                       ws.time_synced ? "true" : "false");
    n += std::snprintf(payload + n, sizeof(payload) - n,
                       ",\"heap_free_bytes\":%u,\"largest_block_bytes\":%u,"
                       "\"schema_version\":1}",
                       static_cast<unsigned int>(rs.free_heap),
                       static_cast<unsigned int>(rs.largest_block));
    return httpReplyOk(req, rid, payload);
}

// --- GET /health -------------------------------------------------------------
static esp_err_t healthHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    const WifiStatus ws = networkManagerStatus();
    const bool healthy = cameraManagerInitialised() &&
                         (ws.state == WifiState::Connected ||
                          ws.state == WifiState::Off);
    char payload[200] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"healthy\":%s,\"camera_initialised\":%s,"
                  "\"wifi_state\":\"%s\",\"schema_version\":1}",
                  healthy ? "true" : "false",
                  cameraManagerInitialised() ? "true" : "false",
                  wifiStateName(ws.state));
    return httpReplyOk(req, rid, payload);
}

// --- GET /metrics ------------------------------------------------------------
static esp_err_t metricsHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    ResourceSnapshot rs{};
    resourceMonitorSnapshot(rs);
    const CameraRecoveryStats crs = cameraRecoveryGetStats();
    const FrameManagerStats fms = frameManagerGetStats();
    char payload[720] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"uptime_s\":%u,\"heap_free_bytes\":%u,"
                  "\"heap_min_bytes\":%u,\"heap_largest_bytes\":%u,"
                  "\"psram_free_bytes\":%u,\"psram_largest_bytes\":%u,"
                  "\"abnormal_boots\":%u,\"boot_count\":%u,"
                  "\"frame_captured\":%u,\"frame_invalid_dropped\":%u,"
                  "\"frame_pool_dropped\":%u,\"frame_queue_dropped\":%u,"
                  "\"frame_backpressure_waits\":%u,"
                  "\"frame_leases_taken\":%u,\"frame_leases_released\":%u,"
                  "\"cam_null_frames\":%u,\"cam_recovery_attempts\":%u,"
                  "\"cam_recovered\":%u,\"cam_failed_events\":%u,"
                  "\"schema_version\":1}",
                  static_cast<unsigned>(rs.uptime_s),
                  static_cast<unsigned>(rs.free_heap),
                  static_cast<unsigned>(rs.min_free_heap),
                  static_cast<unsigned>(rs.largest_block),
                  static_cast<unsigned>(rs.psram_free),
                  static_cast<unsigned>(rs.psram_largest),
                  static_cast<unsigned>(rs.abnormal_boots),
                  static_cast<unsigned>(rs.boot_count),
                  static_cast<unsigned>(fms.frames_captured),
                  static_cast<unsigned>(fms.invalid_dropped),
                  static_cast<unsigned>(fms.pool_dropped),
                  static_cast<unsigned>(fms.queue_dropped),
                  static_cast<unsigned>(fms.backpressure_waits),
                  static_cast<unsigned>(fms.leases_taken),
                  static_cast<unsigned>(fms.leases_released),
                  static_cast<unsigned>(crs.null_frames),
                  static_cast<unsigned>(crs.recovery_attempts),
                  static_cast<unsigned>(crs.recovered),
                  static_cast<unsigned>(crs.failed_events));
    return httpReplyOk(req, rid, payload);
}

// --- POST /device/restart ----------------------------------------------------
static esp_err_t deviceRestartHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (requireCsrf(req) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                              "CSRF token missing or invalid");
    }
    httpReplyOk(req, rid, "{\"restarting\":true}");
    ESP_LOGW(TAG, "device restart requested via API");
    VC_RING_WARN("web", "device restart requested via API");
    esp_restart();
    return ESP_OK;
}

// --- GET /camera/status ------------------------------------------------------
static esp_err_t cameraStatusHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    char payload[260] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"initialised\":%s,\"sensor\":\"%s\","
                  "\"framesize\":%d,\"quality\":%d,"
                  "\"jpeg_capacity_bytes\":%u,\"schema_version\":1}",
                  cameraManagerInitialised() ? "true" : "false",
                  cameraManagerSensorName(),
                  static_cast<int>(cameraManagerFramesize()),
                  cameraManagerQuality(),
                  static_cast<unsigned>(cameraManagerJpegCapacity()));
    return httpReplyOk(req, rid, payload);
}

// --- GET /camera/settings ----------------------------------------------------
static esp_err_t cameraSettingsGetHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    char payload[200] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"framesize\":%d,\"quality\":%d,\"schema_version\":1}",
                  static_cast<int>(cameraManagerFramesize()),
                  cameraManagerQuality());
    return httpReplyOk(req, rid, payload);
}

// --- PUT /camera/settings ----------------------------------------------------
static esp_err_t cameraSettingsPutHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (requireCsrf(req) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                              "CSRF token missing or invalid");
    }
    char body[128] = {};
    size_t blen = 0;
    if (readJsonBody(req, body, sizeof(body), blen) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "invalid JSON body");
    }
    char framesize_str[16] = {};
    char quality_str[8] = {};
    bool have_fs = jsonScalar(body, "\"framesize\"", framesize_str,
                              sizeof(framesize_str));
    bool have_q = jsonScalar(body, "\"quality\"", quality_str,
                             sizeof(quality_str));
    if (!have_fs && !have_q) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "framesize or quality required");
    }
    esp_err_t err = ESP_OK;
    if (have_fs) {
        framesize_t fs = FRAMESIZE_VGA;
        // Two accepted shapes (both contract-valid): a symbolic name
        // ("VGA", what curl tests send) or the framesize_t index 0..15
        // (what the webui sends — GET returns the same number, and the
        // Live View slider is 0..15). The old quoted-only parser rejected
        // numbers outright, so the webui's PUT never applied anything.
        const bool numeric = (framesize_str[0] >= '0' && framesize_str[0] <= '9');
        if (numeric) {
            char* endp = nullptr;
            const long idx = std::strtol(framesize_str, &endp, 10);
            if (endp == nullptr || *endp != '\0' || idx < 0 || idx > 15) {
                return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                      "framesize index out of range 0..15");
            }
            fs = static_cast<framesize_t>(idx);
        } else if (std::strcmp(framesize_str, "QVGA") == 0) fs = FRAMESIZE_QVGA;
        else if (std::strcmp(framesize_str, "VGA") == 0) fs = FRAMESIZE_VGA;
        else if (std::strcmp(framesize_str, "SVGA") == 0) fs = FRAMESIZE_SVGA;
        else if (std::strcmp(framesize_str, "XGA") == 0) fs = FRAMESIZE_XGA;
        else if (std::strcmp(framesize_str, "SXGA") == 0) fs = FRAMESIZE_SXGA;
        else if (std::strcmp(framesize_str, "UXGA") == 0) fs = FRAMESIZE_UXGA;
        else {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "unknown framesize");
        }
        err = cameraManagerSetFramesize(fs);
        if (err != ESP_OK) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "framesize not supported");
        }
    }
    if (have_q && err == ESP_OK) {
        int q = std::atoi(quality_str);
        if (q < 0 || q > 63) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "quality out of range 0..63");
        }
        err = cameraManagerSetQuality(q);
        if (err != ESP_OK) {
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "quality not applied");
        }
    }
    char payload[200] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"framesize\":%d,\"quality\":%d,\"schema_version\":1}",
                  static_cast<int>(cameraManagerFramesize()),
                  cameraManagerQuality());
    return httpReplyOk(req, rid, payload);
}

// --- POST /camera/capture ----------------------------------------------------
static esp_err_t cameraCaptureHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (!cameraManagerInitialised()) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "camera not initialised");
    }
    // NOTE: this handler does NOT auto-start the frame manager (it used to).
    // Boot tests deliberately stop capture around camera teardowns, and an
    // HTTP request resurrecting the capture task in that window raced
    // esp_camera_deinit() — reproduced as panics (main.cpp:409-413). Capture
    // is owned by main/recovery; when it is stopped, frameAcquire simply
    // times out and we answer "no frame available".
    // Acquire a frame lease from the frame manager (sole fb owner, rule 9).
    FrameLease lease;
    if (!frameAcquire(0, lease, /*timeout_ms=*/2000)) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "no frame available");
    }
    // Return the JPEG as image/jpeg. The lease is released after the
    // response is sent (httpd_resp_send copies the data internally).
    esp_err_t err = httpReplyBytes(req, "image/jpeg", lease.data,
                                   lease.len, "no-cache");
    frameRelease(lease);
    return err;
}

// --- GET /storage -----------------------------------------------------------
// Single dispatcher for all storage actions. Route table has 16 handler
// limit (14 Phase 1 + catch-all + 1 storage), so all storage actions share
// one URI distinguished by ?action= query param.
//   ?action=status          -> capacity/mount state
//   ?action=files&offset=N&limit=M&category=C  -> paginated media list
//   ?action=file&id=med-... -> file metadata (GET) / delete (DELETE+CSRF)
//   ?action=rebuild         -> rebuild indexes (POST+CSRF)
//   ?action=retention       -> GET/PUT retention policy (PUT+CSRF)
static esp_err_t storageHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (!visioncam::storageManagerAvailable()) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "storage not available");
    }
    char qs[128] = {};
    size_t qs_size = sizeof(qs);
    if (httpd_req_get_url_query_str(req, qs, qs_size) != ESP_OK) qs[0] = '\0';
    char action[16] = {};
    httpd_query_key_value(qs, "action", action, sizeof(action));

    if (std::strcmp(action, "status") == 0) {
        visioncam::StorageStatus ss{};
        if (visioncam::storageGetStatus(&ss) != ESP_OK) {
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "storage status failed");
        }
        char payload[260] = {};
        std::snprintf(payload, sizeof(payload),
                      "{\"mounted\":%s,\"index_loaded\":%s,"
                      "\"total_bytes\":%llu,\"used_bytes\":%llu,"
                      "\"free_bytes\":%llu,\"media_count\":%u,"
                      "\"schema_version\":1}",
                      ss.mounted ? "true" : "false",
                      ss.index_loaded ? "true" : "false",
                      static_cast<unsigned long long>(ss.total_bytes),
                      static_cast<unsigned long long>(ss.used_bytes),
                      static_cast<unsigned long long>(ss.free_bytes),
                      static_cast<unsigned int>(ss.media_count));
        return httpReplyOk(req, rid, payload);
    }
    if (std::strcmp(action, "files") == 0) {
        uint32_t offset = 0, limit = 20;
        int category = -1;
        char cat_val[16] = {};
        if (httpd_query_key_value(qs, "offset", cat_val, sizeof(cat_val)) == ESP_OK)
            offset = static_cast<uint32_t>(std::atoi(cat_val));
        cat_val[0] = '\0';
        if (httpd_query_key_value(qs, "limit", cat_val, sizeof(cat_val)) == ESP_OK)
            limit = static_cast<uint32_t>(std::atoi(cat_val));
        cat_val[0] = '\0';
        if (httpd_query_key_value(qs, "category", cat_val, sizeof(cat_val)) == ESP_OK)
            category = std::atoi(cat_val);
        if (limit > 100) limit = 100;
        // Small stack arrays to stay within HTTP server's 4 KB task stack.
        visioncam::MediaRecord recs[4] = {};
        size_t count = 0;
        bool has_more = false;
        visioncam::MediaCategory cat =
            (category < 0 || category > 3)
                ? static_cast<visioncam::MediaCategory>(-1)
                : static_cast<visioncam::MediaCategory>(category);
        if (visioncam::storageListMedia(cat, offset, limit, recs, 4,
                                        &count, &has_more) != ESP_OK) {
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "list media failed");
        }
        char payload[512] = {};
        size_t n = 0;
        n += std::snprintf(payload + n, sizeof(payload) - n,
                           "{\"has_more\":%s,\"count\":%u,\"offset\":%u,"
                           "\"limit\":%u,\"items\":[",
                           has_more ? "true" : "false",
                           static_cast<unsigned int>(count),
                           static_cast<unsigned int>(offset),
                           static_cast<unsigned int>(limit));
        for (size_t i = 0; i < count; ++i) {
            if (i > 0) n += std::snprintf(payload + n, sizeof(payload) - n, ",");
            n += std::snprintf(payload + n, sizeof(payload) - n,
                               "{\"media_id\":\"%s\",\"type\":%d,"
                               "\"category\":%d,\"mime\":\"%s\","
                               "\"size\":%u,\"created_at\":\"%s\","
                               "\"event_id\":\"%s\",\"duration_ms\":%u,"
                               "\"status\":\"%s\"}",
                               recs[i].media_id,
                               static_cast<int>(recs[i].type),
                               static_cast<int>(recs[i].category),
                               recs[i].mime,
                               static_cast<unsigned int>(recs[i].size),
                               recs[i].created_at,
                               recs[i].event_id,
                               static_cast<unsigned int>(recs[i].duration_ms),
                               recs[i].status);
        }
        n += std::snprintf(payload + n, sizeof(payload) - n, "]}");
        return httpReplyOk(req, rid, payload);
    }
    if (std::strcmp(action, "file") == 0) {
        char media_id[32] = {};
        httpd_query_key_value(qs, "id", media_id, sizeof(media_id));
        if (media_id[0] == '\0') {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "media id required");
        }
        if (req->method == HTTP_DELETE) {
            if (requireCsrf(req) != ESP_OK) {
                return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                                      "CSRF token missing or invalid");
            }
            const esp_err_t derr = visioncam::storageDeleteMedia(media_id);
            if (derr == ESP_ERR_NOT_FOUND) {
                return httpReplyError(req, rid, ErrCode::NotFound,
                                      "media not found");
            }
            if (derr != ESP_OK) {
                return httpReplyError(req, rid, ErrCode::SdWriteFailed,
                                      "delete failed");
            }
            return httpReplyOk(req, rid, "{\"deleted\":true}");
        }
        // GET file metadata
        visioncam::MediaRecord rec{};
        if (visioncam::storageLookupMedia(media_id, &rec) != ESP_OK) {
            return httpReplyError(req, rid, ErrCode::NotFound,
                                  "media not found");
        }
        char payload[300] = {};
        std::snprintf(payload, sizeof(payload),
                      "{\"media_id\":\"%s\",\"type\":%d,\"category\":%d,"
                      "\"mime\":\"%s\",\"size\":%u,\"created_at\":\"%s\","
                      "\"event_id\":\"%s\",\"duration_ms\":%u,"
                      "\"status\":\"%s\",\"schema_version\":1}",
                      rec.media_id,
                      static_cast<int>(rec.type),
                      static_cast<int>(rec.category),
                      rec.mime,
                      static_cast<unsigned int>(rec.size),
                      rec.created_at,
                      rec.event_id,
                      static_cast<unsigned int>(rec.duration_ms),
                      rec.status);
        return httpReplyOk(req, rid, payload);
    }
    if (std::strcmp(action, "rebuild") == 0) {
        if (req->method != HTTP_POST) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "POST required");
        }
        if (requireCsrf(req) != ESP_OK) {
            return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                                  "CSRF token missing or invalid");
        }
        if (visioncam::storageRebuildIndex() != ESP_OK) {
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "rebuild failed");
        }
        return httpReplyOk(req, rid, "{\"rebuilt\":true}");
    }
    if (std::strcmp(action, "retention") == 0) {
        if (req->method == HTTP_GET) {
            visioncam::RetentionPolicy rp{};
            if (visioncam::storageGetRetention(&rp) != ESP_OK) {
                return httpReplyError(req, rid, ErrCode::Internal,
                                      "retention read failed");
            }
            char payload[200] = {};
            retentionJson(rp, payload, sizeof(payload));
            return httpReplyOk(req, rid, payload);
        }
        // PUT — SPEC 7 update process: parse -> validate -> candidate
        // (current policy, fields overlaid) -> persist (NVS, in the
        // manager) -> apply -> return the sanitised effective policy.
        if (requireCsrf(req) != ESP_OK) {
            return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                                  "CSRF token missing or invalid");
        }
        char body[256] = {};
        size_t blen = 0;
        if (readJsonBody(req, body, sizeof(body), blen) != ESP_OK) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "invalid JSON body");
        }
        visioncam::RetentionPolicy rp{};
        if (visioncam::storageGetRetention(&rp) != ESP_OK) {
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "retention read failed");
        }
        // Candidate starts at the CURRENT policy; only fields present in
        // the body are overlaid (a partial PUT must not reset the rest —
        // the old handler hard-coded the day defaults on every write).
        bool any = false;
        char val[16] = {};
        if (jsonScalar(body, "\"max_used_percent\"", val, sizeof(val))) {
            if (!parseU32Strict(val, rp.max_used_percent)) {
                return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                      "max_used_percent must be 0..100");
            }
            any = true;
        }
        val[0] = '\0';
        if (jsonScalar(body, "\"enabled\"", val, sizeof(val))) {
            if (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0)
                rp.enabled = true;
            else if (std::strcmp(val, "false") == 0 || std::strcmp(val, "0") == 0)
                rp.enabled = false;
            else {
                return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                      "enabled must be true or false");
            }
            any = true;
        }
        struct { const char* key; uint32_t* dst; } day_fields[] = {
            {"\"retention_days_manual\"",  &rp.retention_days_manual},
            {"\"retention_days_motion\"",  &rp.retention_days_motion},
            {"\"retention_days_detection\"", &rp.retention_days_detection},
            {"\"retention_days_timelapse\"", &rp.retention_days_timelapse},
        };
        for (auto& f : day_fields) {
            val[0] = '\0';
            if (!jsonScalar(body, f.key, val, sizeof(val))) continue;
            if (!parseU32Strict(val, *f.dst)) {
                return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                      "retention days must be 1..3650");
            }
            any = true;
        }
        if (!any) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "no retention fields in body");
        }
        const esp_err_t serr = visioncam::storageSetRetention(&rp);
        if (serr == ESP_ERR_INVALID_ARG) {
            // Range validation inside the manager (single source of truth).
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "retention value out of range");
        }
        if (serr != ESP_OK) {
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "retention write failed");
        }
        // Return the sanitised EFFECTIVE policy (what actually persisted),
        // not an "updated" flag — SPEC 7 step 7.
        visioncam::RetentionPolicy effective{};
        if (visioncam::storageGetRetention(&effective) != ESP_OK) {
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "retention read-back failed");
        }
        char payload[200] = {};
        retentionJson(effective, payload, sizeof(payload));
        return httpReplyOk(req, rid, payload);
    }
    return httpReplyError(req, rid, ErrCode::ValidationFailed,
                          "unknown action");
}

}  // namespace visioncam

namespace visioncam {

// ===========================================================================
// server lifecycle
// ===========================================================================

static httpd_handle_t s_server = nullptr;

esp_err_t webServerStart() {
    if (s_server != nullptr) return ESP_ERR_INVALID_STATE;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.stack_size = kHttpServerStack;
    config.max_open_sockets = kHttpMaxOpenSockets;
    config.max_uri_handlers = 24;   // 14 Phase 1 routes + 6 Phase 2 storage routes + catch-all
    config.lru_purge_enable = kHttpLruPurge;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.global_user_ctx = nullptr;
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        s_server = nullptr;
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "HTTP server on port 80, max_open_sockets=%d, stack=%d B",
             kHttpMaxOpenSockets, kHttpServerStack);
    VC_RING_INFO("web", "HTTP server started");
    return ESP_OK;
}

esp_err_t webServerStop() {
    if (s_server == nullptr) return ESP_OK;
    esp_err_t err = httpd_stop(s_server);
    s_server = nullptr;
    if (err != ESP_OK) return err;
    VC_RING_INFO("web", "HTTP server stopped");
    return ESP_OK;
}

bool webServerRunning() { return s_server != nullptr; }

// --- route table ------------------------------------------------------------
static const httpd_uri_t s_routes[] = {
    // Auth
    {"/auth/session", HTTP_GET, authSessionHandler, nullptr},
    {"/auth/login", HTTP_POST, authLoginHandler, nullptr},
    {"/auth/logout", HTTP_POST, authLogoutHandler, nullptr},
    {"/auth/complete-first-login", HTTP_POST, authCompleteFirstLoginHandler,
     nullptr},
    {"/auth/change-password", HTTP_POST, authChangePasswordHandler, nullptr},
    // Device / status
    {"/device", HTTP_GET, deviceHandler, nullptr},
    {"/status", HTTP_GET, statusHandler, nullptr},
    {"/health", HTTP_GET, healthHandler, nullptr},
    {"/metrics", HTTP_GET, metricsHandler, nullptr},
    {"/device/restart", HTTP_POST, deviceRestartHandler, nullptr},
    // Camera
    {"/camera/status", HTTP_GET, cameraStatusHandler, nullptr},
    {"/camera/settings", HTTP_GET, cameraSettingsGetHandler, nullptr},
    {"/camera/settings", HTTP_PUT, cameraSettingsPutHandler, nullptr},
    {"/camera/capture", HTTP_POST, cameraCaptureHandler, nullptr},
    // Storage
    {"/storage", HTTP_GET, storageHandler, nullptr},
    {"/storage", HTTP_DELETE, storageHandler, nullptr},
    {"/storage", HTTP_POST, storageHandler, nullptr},
    {"/storage", HTTP_PUT, storageHandler, nullptr},
    // Catch-all static asset server (SPA fallback for unknown routes).
    // Specific API routes are registered above and match first.
    {"/*", HTTP_GET, assetHandler, nullptr},
    {nullptr, static_cast<httpd_method_t>(0), nullptr, nullptr},
};

esp_err_t webServerRegisterRoutes() {
    if (s_server == nullptr) return ESP_ERR_INVALID_STATE;
    for (const httpd_uri_t* r = s_routes; r->uri != nullptr; ++r) {
        esp_err_t err = httpd_register_uri_handler(s_server, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s %d failed: %s", r->uri,
                     static_cast<int>(r->method), esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "route %s %d", r->uri, static_cast<int>(r->method));
    }
    VC_RING_INFO("web", "routes registered");
    return ESP_OK;
}

}  // namespace visioncam