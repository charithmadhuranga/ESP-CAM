// web_server.cpp — see web_server.h.
//
// esp_http_server core (SPEC 6.0): one instance, lru_purge on,
// max_open_sockets = 6 (4 API clients + 2 stream slots — smaller than R1's
// 7), task stack from VC_STACK_HTTPD in tasks.h. Every JSON response uses
// the {ok,data,error,request_id} envelope with a monotonic per-boot
// request_id.
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
#include <cstdlib>
#include <cstring>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "auth.h"
#include "camera_manager.h"
#include "camera_recovery.h"
#include "frame_manager.h"   // live stream: leases only — AGENTS rule 9
#include "network_manager.h"
#include "resource_monitor.h"
#include "ring_log.h"
#include "storage_manager.h"
#include "snapshot_manager.h"
#include "wall_clock.h"   // kWallClockEraThreshold — snapshot time gate
#include "web_assets_manifest.h"
#include "recording_manager.h"
#include "playback.h"
#include "motion_manager.h"
// #include "timelapse_manager.h"  (descoped with Phase 5/6 — see CMakeLists.txt)
// #include "ai_manager.h"  (descoped with Phase 5/6 — see CMakeLists.txt)
#include "mqtt_manager.h"   // Phase 7: /integrations/mqtt + HA discovery
#include "http_upload_storage_backend.h"   // Phase 8: /integrations/network-storage

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

void skipJsonWhitespace(const char*& cursor, const char* end) {
    while (cursor < end &&
           (*cursor == ' ' || *cursor == '\n' ||
            *cursor == '\r' || *cursor == '\t')) {
        ++cursor;
    }
}

bool appendUtf8(uint32_t codepoint, char* out, size_t cap, size_t& length) {
    uint8_t bytes[4] = {};
    size_t count = 0;
    if (codepoint == 0 || codepoint < 0x20 ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF) ||
        codepoint > 0x10FFFF) return false;
    if (codepoint <= 0x7F) {
        bytes[0] = static_cast<uint8_t>(codepoint);
        count = 1;
    } else if (codepoint <= 0x7FF) {
        bytes[0] = static_cast<uint8_t>(0xC0 | (codepoint >> 6));
        bytes[1] = static_cast<uint8_t>(0x80 | (codepoint & 0x3F));
        count = 2;
    } else if (codepoint <= 0xFFFF) {
        bytes[0] = static_cast<uint8_t>(0xE0 | (codepoint >> 12));
        bytes[1] = static_cast<uint8_t>(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[2] = static_cast<uint8_t>(0x80 | (codepoint & 0x3F));
        count = 3;
    } else {
        bytes[0] = static_cast<uint8_t>(0xF0 | (codepoint >> 18));
        bytes[1] = static_cast<uint8_t>(0x80 | ((codepoint >> 12) & 0x3F));
        bytes[2] = static_cast<uint8_t>(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[3] = static_cast<uint8_t>(0x80 | (codepoint & 0x3F));
        count = 4;
    }
    if (length + count >= cap) return false;
    std::memcpy(out + length, bytes, count);
    length += count;
    return true;
}

bool parseJsonString(const char*& cursor, const char* end, char* out,
                     size_t cap, size_t& length) {
    if (cursor >= end || *cursor++ != '"') return false;
    length = 0;
    while (cursor < end) {
        const uint8_t ch = static_cast<uint8_t>(*cursor++);
        if (ch == '"') {
            if (length >= cap) return false;
            out[length] = '\0';
            return true;
        }
        if (ch < 0x20) return false;
        if (ch != '\\') {
            if (length + 1 >= cap) return false;
            out[length++] = static_cast<char>(ch);
            continue;
        }
        if (cursor >= end) return false;
        const char escaped = *cursor++;
        if (escaped == '"' || escaped == '\\' || escaped == '/') {
            if (length + 1 >= cap) return false;
            out[length++] = escaped;
            continue;
        }
        if (escaped != 'u' || end - cursor < 4) return false;
        uint32_t codepoint = 0;
        for (int i = 0; i < 4; ++i) {
            const char digit = *cursor++;
            codepoint <<= 4;
            if (digit >= '0' && digit <= '9') codepoint |= digit - '0';
            else if (digit >= 'a' && digit <= 'f') codepoint |= digit - 'a' + 10;
            else if (digit >= 'A' && digit <= 'F') codepoint |= digit - 'A' + 10;
            else return false;
        }
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            if (end - cursor < 6 || cursor[0] != '\\' || cursor[1] != 'u') return false;
            cursor += 2;
            uint32_t low = 0;
            for (int i = 0; i < 4; ++i) {
                const char digit = *cursor++;
                low <<= 4;
                if (digit >= '0' && digit <= '9') low |= digit - '0';
                else if (digit >= 'a' && digit <= 'f') low |= digit - 'a' + 10;
                else if (digit >= 'A' && digit <= 'F') low |= digit - 'A' + 10;
                else return false;
            }
            if (low < 0xDC00 || low > 0xDFFF) return false;
            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            return false;
        }
        if (!appendUtf8(codepoint, out, cap, length)) return false;
    }
    return false;
}

bool parseWifiConfigJson(const char* body, size_t body_len,
                         char* ssid, size_t ssid_cap,
                         char* pass, size_t pass_cap) {
    const char* cursor = body;
    const char* end = body + body_len;
    bool have_ssid = false;
    bool have_pass = false;
    skipJsonWhitespace(cursor, end);
    if (cursor >= end || *cursor++ != '{') return false;
    for (;;) {
        char key[16] = {};
        size_t key_len = 0;
        size_t value_len = 0;
        skipJsonWhitespace(cursor, end);
        if (!parseJsonString(cursor, end, key, sizeof(key), key_len)) return false;
        skipJsonWhitespace(cursor, end);
        if (cursor >= end || *cursor++ != ':') return false;
        skipJsonWhitespace(cursor, end);

        const bool is_ssid = std::strcmp(key, "ssid") == 0;
        const bool is_pass = std::strcmp(key, "password") == 0;
        if ((!is_ssid && !is_pass) ||
            (is_ssid && have_ssid) || (is_pass && have_pass)) return false;
        const size_t value_cap = is_ssid ? ssid_cap : pass_cap;
        char* value_out = is_ssid ? ssid : pass;
        if (!parseJsonString(cursor, end, value_out, value_cap, value_len)) return false;
        if (is_ssid) have_ssid = true;
        else have_pass = true;

        skipJsonWhitespace(cursor, end);
        if (cursor >= end) return false;
        if (*cursor == '}') {
            ++cursor;
            break;
        }
        if (*cursor++ != ',') return false;
    }
    skipJsonWhitespace(cursor, end);
    return cursor == end && have_ssid && have_pass &&
           ssid[0] != '\0' &&
           (pass[0] == '\0' ||
            (std::strlen(pass) >= 8 && std::strlen(pass) <= 63));
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

// Strict float parser for JSON scalars such as confidence_threshold
// ("0.6"). Syntax only — the caller applies the range check and reports
// the domain-specific message. Rejects trailing junk and empty strings;
// the caller's range comparison also rejects NaN and infinities.
bool parseFloatStrict(const char* s, float& out) {
    if (s == nullptr || *s == '\0') return false;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end == s || end == nullptr || *end != '\0') return false;
    out = v;
    return true;
}

// Model IDs become a filesystem path segment
// (/sdcard/models/<dir>/<id>.tflite) AND are echoed back in JSON, so only
// [A-Za-z0-9_-], 1..48 chars is accepted. This rejects traversal ("../",
// absolute paths), quotes and control chars — defence in depth alongside
// storagePathIsUnderSd (AGENTS rule 15 / SPEC §6.0).
bool parseModelId(const char* in, char* out, size_t cap) {
    if (in == nullptr || out == nullptr || cap < 2) return false;
    size_t n = 0;
    for (const char* p = in; *p != '\0'; ++p) {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok) return false;
        if (++n >= cap) return false;
    }
    if (n == 0) return false;
    std::memcpy(out, in, n);
    out[n] = '\0';
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
                        // Send gzip data in chunks (avoids null-byte truncation)
                   const uint8_t* data = web_assets[i].data;
            size_t remaining = web_assets[i].comp_size;
            size_t offset = 0;
            while (remaining > 0) {
                size_t chunk = std::min<size_t>(remaining, 1024);
                if (httpd_resp_send_chunk(req,
                                          reinterpret_cast<const char*>(data + offset),
                                          chunk) != ESP_OK) {
                    return ESP_FAIL;
                }
                offset += chunk;
                remaining -= chunk;
            }
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_OK;
        }
    }
    // Not found — serve index.html as SPA fallback (client-side routing).
    // The manifest contains "/index.html", not "/", so look it up by its
    // real URL. If even that is absent, 404.
    const char* index_urls[2] = {"/index.html", "/"};
    for (size_t u = 0; u < 2; ++u) {
        const char* index_url = index_urls[u];
        for (size_t i = 0; i < web_assets_count; ++i) {
            if (std::strcmp(index_url, web_assets[i].url) == 0) {
            httpd_resp_set_type(req, "text/html");
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
            char len_buf[16];
            std::snprintf(len_buf, sizeof(len_buf), "%u",
                          static_cast<unsigned>(web_assets[i].comp_size));
                        // Send gzip data in chunks (avoids null-byte truncation)
                   const uint8_t* data = web_assets[i].data;
            size_t remaining = web_assets[i].comp_size;
            size_t offset = 0;
            while (remaining > 0) {
                size_t chunk = std::min<size_t>(remaining, 1024);
                if (httpd_resp_send_chunk(req,
                                          reinterpret_cast<const char*>(data + offset),
                                          chunk) != ESP_OK) {
                    return ESP_FAIL;
                }
                offset += chunk;
                remaining -= chunk;
            }
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_OK;
        }
    }
    }
    return httpd_resp_set_status(req, "404 Not Found") == ESP_OK
               ? httpd_resp_send(req, "Not found", 9)
               : ESP_FAIL;
}

// Monotonic per-boot request_id (SPEC 6.0).
std::atomic<uint32_t> s_request_id{0};

// Frees a heap block when it goes out of scope. Handlers here have many
// exit paths (every error return), and firmware/AGENTS.md requires resources
// be released on EVERY one — a manual free before each return is exactly the
// pattern that leaks. std::free(nullptr) is a no-op, so an uninitialised
// guard is still safe.
struct HeapFree {
    void* p;
    ~HeapFree() { std::free(p); }
};

// Envelope cap for one response's "data" field. Every handler builds its
// payload in a buffer of its own and every payload is bounded by a
// compile-time constant (AGENTS rule 13), so this is a structural bound,
// never one taken from a request. Refusing with a valid error envelope beats
// emitting truncated — invalid — JSON.
constexpr size_t kEnvelopeMaxPayload = 4096;

// Last resort when the real envelope cannot be assembled (payload over the
// cap, or out of memory). Always valid JSON and deliberately carries no
// payload. HTTP status may already have been set by httpReplyError().
esp_err_t sendEnvelopeFallback(httpd_req_t* req, uint32_t request_id) {
    char small[192] = {};
    const int n = std::snprintf(
        small, sizeof(small),
        "{\"ok\":false,\"request_id\":%u,"
        "\"error\":{\"code\":%u,\"message\":\"response could not be built\"},"
        "\"data\":null}",
        static_cast<unsigned int>(request_id),
        static_cast<unsigned>(ErrCode::Internal));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(small)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, small, n);
}

// Sends the full JSON envelope. payload is inserted verbatim as the "data"
// field (must be valid JSON or "null").
//
// The framing is assembled into a HEAP buffer of the exact needed size, not
// a fixed stack buffer. The previous fixed char[512] silently truncated
// every response longer than 511 bytes — observed cutting /metrics
// mid-string and breaking /storage?action=files as soon as it carried four
// items — and the truncation was invisible because snprintf reports it only
// if you ask. This runs on the HTTP task, so sizing the stack for the
// largest legal payload would just trade one hazard for another.
esp_err_t sendEnvelope(httpd_req_t* req, bool ok, uint32_t request_id,
                       ErrCode code, const char* message,
                       const char* payload) {
    if (payload == nullptr) payload = "null";
    const size_t plen = std::strlen(payload);
    if (plen > kEnvelopeMaxPayload) {
        ESP_LOGE(TAG, "envelope payload of %u B exceeds the %u B cap",
                 static_cast<unsigned>(plen),
                 static_cast<unsigned>(kEnvelopeMaxPayload));
        return sendEnvelopeFallback(req, request_id);
    }

    char head[256] = {};
    int h;
    if (code != ErrCode::Ok && message != nullptr) {
        h = std::snprintf(head, sizeof(head),
                          "{\"ok\":%s,\"request_id\":%u,"
                          "\"error\":{\"code\":%u,\"message\":\"%s\"},\"data\":",
                          ok ? "true" : "false",
                          static_cast<unsigned int>(request_id),
                          static_cast<unsigned>(code), message);
    } else {
        h = std::snprintf(head, sizeof(head),
                          "{\"ok\":%s,\"request_id\":%u,\"error\":null,\"data\":",
                          ok ? "true" : "false",
                          static_cast<unsigned int>(request_id));
    }
    if (h < 0 || static_cast<size_t>(h) >= sizeof(head)) {
        return sendEnvelopeFallback(req, request_id);
    }

    const size_t total = static_cast<size_t>(h) + plen + 1;  // + '}'
    char* buf = static_cast<char*>(std::malloc(total));
    if (buf == nullptr) return sendEnvelopeFallback(req, request_id);
    std::memcpy(buf, head, static_cast<size_t>(h));
    std::memcpy(buf + h, payload, plen);
    buf[h + plen] = '}';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, buf, static_cast<ssize_t>(total));
    std::free(buf);
    return err;
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
    case ErrCode::TimeUnsynchronised:
    case ErrCode::MqttUnavailable:
    case ErrCode::NetworkStorageUnavailable:
    case ErrCode::WifiUnavailable:             status = "503 Service Unavailable"; break;
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
                // Send gzip data in chunks (avoids null-byte truncation)
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
    // TWO separate Set-Cookie headers — RFC 6265 allows one cookie per
    // header, and httpd_resp_set_hdr() APPENDS to the response header list
    // (httpd_txrx.c:195-198), so calling it twice emits both. The old code
    // comma-joined them into a single header on the wrong assumption that
    // set_hdr replaces; curl and browsers then stored only the first cookie
    // (the session) and never saw visioncam_csrf, so every state-changing
    // request failed CSRF (reproduced 2026-11-21).
    char set_cookie_session[192] = {};
    std::snprintf(set_cookie_session, sizeof(set_cookie_session),
                  "%s=%s; Path=/; HttpOnly; SameSite=Lax",
                  kSessionCookie, token_b64);
    char set_cookie_csrf[192] = {};
    std::snprintf(set_cookie_csrf, sizeof(set_cookie_csrf),
                  "%s=%s; Path=/; SameSite=Lax",
                  kCsrfCookie, csrf_b64);   // JS-readable on purpose
    httpd_resp_set_hdr(req, "Set-Cookie", set_cookie_session);
    httpd_resp_set_hdr(req, "Set-Cookie", set_cookie_csrf);
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
    // Two headers, one cookie each (see login handler — RFC 6265).
    httpd_resp_set_hdr(req, "Set-Cookie",
                       "visioncam_session=; Path=/; HttpOnly; SameSite=Lax; "
                       "Max-Age=0");
    httpd_resp_set_hdr(req, "Set-Cookie",
                       "visioncam_csrf=; Path=/; SameSite=Lax; Max-Age=0");
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

// --- POST /wifi/config ------------------------------------------------------
// A setup-AP-only endpoint. Credentials are tested before NVS is updated,
// so a typo cannot replace a working configuration or strand setup access.
static esp_err_t wifiConfigPostHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (requireCsrf(req) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                              "CSRF token missing or invalid");
    }

    const WifiStatus status = networkManagerStatus();
    if (!status.ap_mode || status.configured || status.provision_pending) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "Wi-Fi setup is only available in setup AP mode");
    }

    char body[256] = {};
    size_t body_len = 0;
    if (readJsonBody(req, body, sizeof(body), body_len) != ESP_OK) {
        std::memset(body, 0, sizeof(body));
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "invalid Wi-Fi configuration body");
    }

    char ssid[33] = {};
    char pass[64] = {};
    const bool valid = parseWifiConfigJson(body, body_len, ssid, sizeof(ssid),
                                           pass, sizeof(pass));
    std::memset(body, 0, sizeof(body));

    if (!valid) {
        std::memset(ssid, 0, sizeof(ssid));
        std::memset(pass, 0, sizeof(pass));
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "SSID must be 1..32 bytes; password must be empty or 8..63 bytes");
    }

    const esp_err_t err = networkManagerProvision(ssid, pass);
    std::memset(ssid, 0, sizeof(ssid));
    std::memset(pass, 0, sizeof(pass));
    if (err == ESP_ERR_TIMEOUT) {
        return httpReplyError(req, rid, ErrCode::WifiUnavailable,
                              "Could not join that Wi-Fi network; check its password and use a 2.4 GHz network");
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "invalid Wi-Fi credentials");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "Wi-Fi setup is already in progress or unavailable");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi provisioning failed: %s", esp_err_to_name(err));
        return httpReplyError(req, rid, ErrCode::Internal,
                              "could not save Wi-Fi configuration");
    }

    const esp_err_t response_err = httpReplyOk(
        req, rid,
        "{\"configured\":false,\"provision_pending\":true,"
        "\"connected\":true,\"ap_mode\":false,\"restarting\":true,"
        "\"schema_version\":1}");
    const esp_err_t ap_err = networkManagerStopSetupAp();
    if (ap_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi connected but setup AP shutdown failed: %s",
                 esp_err_to_name(ap_err));
    } else {
        ESP_LOGI(TAG, "verified Wi-Fi setup; restarting to commit credentials");
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return response_err;
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
    // One snprintf, one buffer, one fit check: the previous two-step
    // `n += snprintf(payload + n, ...)` with a size_t accumulator could
    // underflow sizeof(payload) - n and write past the buffer if it ever
    // ran long.
    const int n = std::snprintf(payload, sizeof(payload),
                       "{\"wifi\":{\"state\":\"%s\",\"configured\":%s,"
                       "\"ap_mode\":%s,\"provision_pending\":%s,"
                       "\"radio_up\":%s,\"rssi\":%d,\"connects\":%u,"
                       "\"disconnects\":%u},\"camera\":{\"initialised\":%s,"
                       "\"framesize\":%d},\"time_synced\":%s,"
                       "\"heap_free_bytes\":%u,\"largest_block_bytes\":%u,"
                       "\"storage\":{\"mounted\":%s,\"media_count\":%u},"
                       "\"schema_version\":1}",
                       wifiStateName(ws.state),
                       ws.configured ? "true" : "false",
                       ws.ap_mode ? "true" : "false",
                       ws.provision_pending ? "true" : "false",
                       ws.radio_up ? "true" : "false", ws.rssi,
                       static_cast<unsigned int>(ws.connects),
                       static_cast<unsigned int>(ws.disconnects),
                       cameraManagerInitialised() ? "true" : "false",
                       static_cast<int>(cameraManagerFramesize()),
                       ws.time_synced ? "true" : "false",
                       static_cast<unsigned int>(rs.free_heap),
                       static_cast<unsigned int>(rs.largest_block),
                       storageManagerAvailable() ? "true" : "false",
                       []{
                           visioncam::StorageStatus ss{};
                           if (storageManagerAvailable() &&
                               visioncam::storageGetStatus(&ss) == ESP_OK)
                               return static_cast<unsigned int>(ss.media_count);
                           return 0u;
                       }());
    if (n < 0 || static_cast<size_t>(n) >= sizeof(payload)) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "status payload overflow");
    }
    return httpReplyOk(req, rid, payload);
}

// --- GET /health -------------------------------------------------------------
static esp_err_t healthHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    const WifiStatus ws = networkManagerStatus();
    const bool healthy = cameraManagerInitialised() &&
                         (ws.state == WifiState::Connected ||
                          ws.state == WifiState::Off ||
                          ws.state == WifiState::Ap);
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
// Two bounds, both compile-time (AGENTS rule 13): the payload buffer is a
// single bounded frame (never a growing one), and the task array is capped
// at VC_MAX_TASKS. Every snprintf is followed by a fit check, because a
// silently truncated envelope would be invalid JSON for every client.
constexpr int kMetricsPayloadMax = 1792;
constexpr int kMetricsMaxTasks = VC_MAX_TASKS;

// GET /storage?action=files page shape. ONE constant sizes both the record
// array and the payload buffer, so the JSON can never be truncated by the
// page size: twelve records at up to ~200 B each plus the wrapper fits in
// kFilesPayloadMax with room to spare. SPEC 6.5 / API contract: "limit" is
// a STRICT MAXIMUM, so serving fewer records than requested is legal as
// long as has_more says there are more (listMedia() scans past out_cap to
// compute it) — the client pages on offset.
//
// Both buffers are heap-allocated in the handler: 12 x ~256 B of records
// plus a 3 KB payload must not be sized onto the HTTP task's stack.
constexpr size_t kFilesPageMax = 12;
constexpr int kFilesPayloadMax = 3072;

static esp_err_t metricsHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    ResourceSnapshot rs{};
    resourceMonitorSnapshot(rs);
    const CameraRecoveryStats crs = cameraRecoveryGetStats();
    const FrameManagerStats fms = frameManagerGetStats();
    char payload[kMetricsPayloadMax] = {};
    int n = std::snprintf(payload, sizeof(payload),
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
                  "\"task_count\":%u,\"tasks_reported\":%u,\"tasks\":[",
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
                  static_cast<unsigned>(crs.failed_events),
                  static_cast<unsigned>(rs.task_count),
                  static_cast<unsigned>(rs.tasks_reported));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(payload))
        return httpReplyError(req, rid, ErrCode::Internal,
                              "metrics payload overflow");

    // Task table, worst stack high-water mark first (resource_monitor
    // orders it that way). "hwm" is minimum free stack in BYTES — this is
    // the number docs/ARCHITECTURE.md §4 needs per task, and the only way
    // to see whether the HTTP task's own stack is adequate.
    const uint16_t shown = static_cast<uint16_t>(
        rs.tasks_reported < kMetricsMaxTasks ? rs.tasks_reported
                                             : kMetricsMaxTasks);
    for (uint16_t i = 0; i < shown; ++i) {
        const TaskInfo& t = rs.tasks[i];
        const int m = std::snprintf(
            payload + n, sizeof(payload) - n,
            "%s{\"n\":\"%.15s\",\"hwm\":%u,\"prio\":%u,\"core\":%d,"
            "\"state\":%u}",
            (i == 0) ? "" : ",", t.name,
            static_cast<unsigned>(t.stack_hwm),
            static_cast<unsigned>(t.prio), static_cast<int>(t.core),
            static_cast<unsigned>(t.state));
        if (m < 0 || static_cast<size_t>(n + m) >= sizeof(payload))
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "metrics payload overflow");
        n += m;
    }
    const int tail = std::snprintf(payload + n, sizeof(payload) - n,
                                   "],\"schema_version\":1}");
    if (tail < 0 || static_cast<size_t>(n + tail) >= sizeof(payload))
        return httpReplyError(req, rid, ErrCode::Internal,
                              "metrics payload overflow");
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
    // Subscriber 0 is the smoke-test subscription registered at boot — it
    // stays subscribed for the device's lifetime, so it is always available
    // once the capture task is running.
    FrameLease lease;
    if (!frameAcquire(0, lease, /*timeout_ms=*/5000)) {
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
    char action[32] = {};
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
        if (limit > 100) limit = 100;
        // Page size and payload buffer are a matched pair of compile-time
        // constants (kFilesPageMax / kFilesPayloadMax, above), and BOTH live
        // on the heap rather than the HTTP task's stack: MediaRecord is
        // ~256 B, so a 12-record page is ~3 KB of records plus a ~3 KB
        // payload. Sizing the stack for the largest legal page is the same
        // mistake that overflowed this handler before -- the old
        // `n += snprintf(payload + n, ...)` had no fit check, and once four
        // records were listed `sizeof(payload) - n` underflowed and the next
        // call wrote past the end of a 512-byte stack buffer.
        void* recs_raw = std::calloc(kFilesPageMax,
                                     sizeof(visioncam::MediaRecord));
        HeapFree recs_guard{recs_raw};
        void* payload_raw = std::malloc(kFilesPayloadMax);
        HeapFree payload_guard{payload_raw};
        if (recs_raw == nullptr || payload_raw == nullptr) {
            return httpReplyError(req, rid, ErrCode::Internal, "no memory");
        }
        auto* recs = static_cast<visioncam::MediaRecord*>(recs_raw);
        auto* payload = static_cast<char*>(payload_raw);
        const size_t payload_cap = static_cast<size_t>(kFilesPayloadMax);

        size_t count = 0;
        bool has_more = false;
        visioncam::MediaCategory cat =
            (category < 0 || category > 3)
                ? static_cast<visioncam::MediaCategory>(-1)
                : static_cast<visioncam::MediaCategory>(category);
        if (visioncam::storageListMedia(cat, offset, limit, recs,
                                        kFilesPageMax, &count, &has_more) !=
            ESP_OK) {
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "list media failed");
        }
        int n = std::snprintf(payload, payload_cap,
                       "{\"has_more\":%s,\"count\":%u,\"offset\":%u,"
                       "\"limit\":%u,\"items\":[",
                       has_more ? "true" : "false",
                       static_cast<unsigned int>(count),
                       static_cast<unsigned int>(offset),
                       static_cast<unsigned int>(limit));
        if (n < 0 || static_cast<size_t>(n) >= payload_cap) {
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "files payload overflow");
        }
        for (size_t i = 0; i < count; ++i) {
            const int m = std::snprintf(
                payload + n, payload_cap - static_cast<size_t>(n),
                "%s{\"media_id\":\"%s\",\"type\":%d,"
                "\"category\":%d,\"mime\":\"%s\","
                "\"size\":%u,\"created_at\":\"%s\","
                "\"event_id\":\"%s\",\"duration_ms\":%u,"
                "\"status\":\"%s\"}",
                (i == 0) ? "" : ",",
                recs[i].media_id,
                static_cast<int>(recs[i].type),
                static_cast<int>(recs[i].category),
                recs[i].mime,
                static_cast<unsigned int>(recs[i].size),
                recs[i].created_at,
                recs[i].event_id,
                static_cast<unsigned int>(recs[i].duration_ms),
                recs[i].status);
            if (m < 0 || static_cast<size_t>(n + m) >= payload_cap) {
                return httpReplyError(req, rid, ErrCode::Internal,
                                      "files payload overflow");
            }
            n += m;
        }
        const int tail = std::snprintf(payload + n,
                                       payload_cap - static_cast<size_t>(n),
                                       "]}");
        if (tail < 0 || static_cast<size_t>(n + tail) >= payload_cap) {
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "files payload overflow");
        }
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
    if (std::strcmp(action, "retention-preview") == 0) {
        if (req->method != HTTP_GET) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "GET required");
        }
        // Preview of automatic deletion candidates. Bounded to 32 entries;
        // previewDeletions() scans the in-memory index (never the whole card)
        // and skips anything not "ready" (open segments, active models, etc.).
        // Heap-allocated: 32 x ~164 B = ~5.2 KB would overflow the HTTP
        // task's 8 KB stack if placed there.
        void* cands_raw = std::calloc(32, sizeof(DeletionCandidate));
        HeapFree cands_guard{cands_raw};
        if (cands_raw == nullptr) {
            return httpReplyError(req, rid, ErrCode::Internal, "no memory");
        }
        auto* cands = static_cast<DeletionCandidate*>(cands_raw);
        size_t cand_count = 0;
        if (visioncam::storagePreviewDeletions(cands, 32, &cand_count) !=
            ESP_OK) {
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "retention preview failed");
        }
        void* payload_raw = std::malloc(4096);
        HeapFree payload_guard{payload_raw};
        if (payload_raw == nullptr) {
            return httpReplyError(req, rid, ErrCode::Internal, "no memory");
        }
        auto* payload = static_cast<char*>(payload_raw);
        const size_t cap = 4096;
        int n = std::snprintf(payload, cap,
                              "{\"count\":%u,\"candidates\":[",
                              static_cast<unsigned int>(cand_count));
        if (n < 0 || static_cast<size_t>(n) >= cap) {
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "preview payload overflow");
        }
        for (size_t i = 0; i < cand_count; ++i) {
            const int m = std::snprintf(
                payload + n, cap - static_cast<size_t>(n),
                "%s{\"media_id\":\"%s\",\"category\":%d,\"path\":\"%s\","
                "\"size\":%u,\"age_days\":%u}",
                (i == 0) ? "" : ",",
                cands[i].media_id,
                static_cast<int>(cands[i].category),
                cands[i].path,
                static_cast<unsigned int>(cands[i].size),
                static_cast<unsigned int>(cands[i].age_days));
            if (m < 0 || static_cast<size_t>(n + m) >= cap) {
                return httpReplyError(req, rid, ErrCode::Internal,
                                      "preview payload overflow");
            }
            n += m;
        }
        const int tail = std::snprintf(payload + n,
                                       cap - static_cast<size_t>(n), "]}");
        if (tail < 0 || static_cast<size_t>(n + tail) >= cap) {
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "preview payload overflow");
        }
        return httpReplyOk(req, rid, payload);
    }
    if (std::strcmp(action, "retention-run") == 0) {
        if (req->method != HTTP_POST) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "POST required");
        }
        if (requireCsrf(req) != ESP_OK) {
            return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                                  "CSRF token missing or invalid");
        }
        // Execute retention: delete oldest eligible files until under
        // max_used_percent or retention_days exceeded. Logs every deletion
        // (runRetention logs each media_id with age and category). Refuses
        // anything not "ready" — active/open segments and the only copy of
        // an active model are never deleted (SPEC 9 / plan 02 task 2.4).
        uint32_t deleted = 0;
        const esp_err_t rerr =
            visioncam::storageRunRetention(&deleted);
        if (rerr != ESP_OK) {
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "retention run failed");
        }
        char payload[64] = {};
        std::snprintf(payload, sizeof(payload),
                      "{\"deleted\":%u,\"schema_version\":1}",
                      static_cast<unsigned int>(deleted));
        return httpReplyOk(req, rid, payload);
    }
    return httpReplyError(req, rid, ErrCode::ValidationFailed,
                          "unknown action");
}

// --- GET /recording/status --- (Phase 3 task 3.3)
static esp_err_t recordingStatusHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    RecordingState st{};
    if (visioncam::recordingManagerState(&st) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "recording status failed");
    }
    const char* st_str = (st == RecordingState::Recording) ? "recording"
                                                           : "idle";
    char payload[128] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"state\":\"%s\",\"schema_version\":1}", st_str);
    return httpReplyOk(req, rid, payload);
}

// --- GET /recording/config --- (Phase 3 task 3.3)
static esp_err_t recordingConfigGetHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    RecordingConfig cfg{};
    if (visioncam::recordingManagerGetConfig(&cfg) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "recording config read failed");
    }
    char payload[256] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"enabled\":%s,\"capture_fps\":%u,\"segment_duration_s\":%u,"
                  "\"min_duration_s\":%u,\"max_storage_percent\":%u,"
                  "\"post_event_s\":%u,\"pre_event_s\":%u,"
                  "\"schema_version\":1}",
                  cfg.enabled ? "true" : "false",
                  static_cast<unsigned int>(cfg.capture_fps),
                  static_cast<unsigned int>(cfg.segment_duration_s),
                  static_cast<unsigned int>(cfg.min_duration_s),
                  static_cast<unsigned int>(cfg.max_storage_percent),
                  static_cast<unsigned int>(cfg.post_event_s),
                  static_cast<unsigned int>(cfg.pre_event_s));
    return httpReplyOk(req, rid, payload);
}

// --- PUT /recording/config --- (Phase 3 task 3.3; SPEC 7 update process)
static esp_err_t recordingConfigPutHandler(httpd_req_t* req) {
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
    RecordingConfig cur{};
    if (visioncam::recordingManagerGetConfig(&cur) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "recording config read failed");
    }
    RecordingConfig cand = cur;
    char val[16] = {};
    bool any = false;
    if (jsonScalar(body, "\"enabled\"", val, sizeof(val))) {
        cand.enabled = (std::strcmp(val, "true") == 0 ||
                        std::strcmp(val, "1") == 0);
        any = true;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"capture_fps\"", val, sizeof(val))) {
        uint32_t fps = 0;
        if (!parseU32Strict(val, fps) || fps > 60) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "capture_fps must be 1..60");
        }
        cand.capture_fps = fps;
        any = true;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"segment_duration_s\"", val, sizeof(val))) {
        uint32_t sd = 0;
        if (!parseU32Strict(val, sd) || sd > 3600) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "segment_duration_s must be 1..3600");
        }
        cand.segment_duration_s = sd;
        any = true;
    }
    if (!any) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "no recording fields in body");
    }
    RecordingConfig effective{};
    if (visioncam::recordingManagerSetConfig(&cand, &effective) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "recording config write failed");
    }
    char payload[256] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"enabled\":%s,\"capture_fps\":%u,\"segment_duration_s\":%u,"
                  "\"min_duration_s\":%u,\"max_storage_percent\":%u,"
                  "\"post_event_s\":%u,\"pre_event_s\":%u,"
                  "\"schema_version\":1}",
                  effective.enabled ? "true" : "false",
                  static_cast<unsigned int>(effective.capture_fps),
                  static_cast<unsigned int>(effective.segment_duration_s),
                  static_cast<unsigned int>(effective.min_duration_s),
                  static_cast<unsigned int>(effective.max_storage_percent),
                  static_cast<unsigned int>(effective.post_event_s),
                  static_cast<unsigned int>(effective.pre_event_s));
    return httpReplyOk(req, rid, payload);
}

// --- POST /recording/start --- (Phase 3 task 3.3)
static esp_err_t recordingStartHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (requireCsrf(req) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                              "CSRF token missing or invalid");
    }
    char media_id[32] = {};
    esp_err_t err = visioncam::recordingManagerStartManual(
        media_id, sizeof(media_id));
    if (err != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "recording start failed");
    }
    char payload[80] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"recording_id\":\"%s\",\"started\":true}", media_id);
    return httpReplyOk(req, rid, payload);
}

// --- POST /recording/stop --- (Phase 3 task 3.3)
static esp_err_t recordingStopHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (requireCsrf(req) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                              "CSRF token missing or invalid");
    }
    esp_err_t err = visioncam::recordingManagerStop();
    if (err != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "recording stop failed");
    }
    return httpReplyOk(req, rid, "{\"stopped\":true}");
}

// --- GET /recordings --- (Phase 3 task 3.5; paginated list)
// Dispatches on ?action=metadata|download|play for sub-resources.
static esp_err_t recordingMetadataHandler(httpd_req_t* req);
static esp_err_t recordingDownloadHandler(httpd_req_t* req);
static esp_err_t recordingPlayHandler(httpd_req_t* req);

static esp_err_t recordingsListHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    char qs[128] = {};
    size_t qs_size = sizeof(qs);
    if (httpd_req_get_url_query_str(req, qs, qs_size) != ESP_OK)
        qs[0] = '\0';
    char action[16] = {};
    httpd_query_key_value(qs, "action", action, sizeof(action));

    if (std::strcmp(action, "metadata") == 0)
        return recordingMetadataHandler(req);
    if (std::strcmp(action, "download") == 0)
        return recordingDownloadHandler(req);
    if (std::strcmp(action, "play") == 0)
        return recordingPlayHandler(req);

    // Default: paginated list
    uint32_t offset = 0, limit = 12;
    char val[16] = {};
    if (httpd_query_key_value(qs, "offset", val, sizeof(val)) == ESP_OK)
        offset = static_cast<uint32_t>(std::atoi(val));
    val[0] = '\0';
    if (httpd_query_key_value(qs, "limit", val, sizeof(val)) == ESP_OK)
        limit = static_cast<uint32_t>(std::atoi(val));
    if (limit > 100) limit = 100;

    RecordingInfo recs[12] = {};
    size_t count = 0;
    bool has_more = false;
    if (visioncam::recordingManagerList(offset, limit, recs, 12, &count,
                                        &has_more) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "recordings list failed");
    }
    void* payload_raw = std::malloc(4096);
    HeapFree payload_guard{payload_raw};
    if (payload_raw == nullptr) {
        return httpReplyError(req, rid, ErrCode::Internal, "no memory");
    }
    auto* payload = static_cast<char*>(payload_raw);
    const size_t cap = 4096;
    int n = std::snprintf(payload, cap,
                          "{\"has_more\":%s,\"count\":%u,\"offset\":%u,"
                          "\"limit\":%u,\"items\":[",
                          has_more ? "true" : "false",
                          static_cast<unsigned int>(count),
                          static_cast<unsigned int>(offset),
                          static_cast<unsigned int>(limit));
    if (n < 0 || static_cast<size_t>(n) >= cap) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "recordings payload overflow");
    }
    for (size_t i = 0; i < count; ++i) {
        const int m = std::snprintf(
            payload + n, cap - static_cast<size_t>(n),
            "%s{\"recording_id\":\"%s\",\"category\":\"%s\","
            "\"start_time\":%u,\"duration_s\":%u,\"frame_count\":%u,"
            "\"size_bytes\":%u,\"width\":%u,\"height\":%u,"
            "\"fps\":%u,\"status\":\"%s\",\"recovered\":%s}",
            (i == 0) ? "" : ",",
            recs[i].recording_id,
            recs[i].category,
            static_cast<unsigned int>(recs[i].start_time),
            static_cast<unsigned int>(recs[i].duration_s),
            static_cast<unsigned int>(recs[i].frame_count),
            static_cast<unsigned int>(recs[i].size_bytes),
            static_cast<unsigned int>(recs[i].width),
            static_cast<unsigned int>(recs[i].height),
            static_cast<unsigned int>(recs[i].fps),
            recs[i].status,
            recs[i].recovered ? "true" : "false");
        if (m < 0 || static_cast<size_t>(n + m) >= cap) {
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "recordings payload overflow");
        }
        n += m;
    }
    const int tail = std::snprintf(payload + n, cap - static_cast<size_t>(n),
                                   "]}");
    if (tail < 0 || static_cast<size_t>(n + tail) >= cap) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "recordings payload overflow");
    }
    return httpReplyOk(req, rid, payload);
}

// --- GET /recordings/{id} --- metadata
static esp_err_t recordingMetadataHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    char qs[128] = {};
    size_t qs_size = sizeof(qs);
    if (httpd_req_get_url_query_str(req, qs, qs_size) != ESP_OK)
        qs[0] = '\0';
    char rec_id[32] = {};
    httpd_query_key_value(qs, "id", rec_id, sizeof(rec_id));
    if (rec_id[0] == '\0') {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "recording id required");
    }
    RecordingInfo info{};
    if (visioncam::recordingManagerLookup(rec_id, &info) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::NotFound,
                              "recording not found");
    }
    char payload[256] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"recording_id\":\"%s\",\"category\":\"%s\","
                  "\"start_time\":%u,\"duration_s\":%u,\"frame_count\":%u,"
                  "\"size_bytes\":%u,\"width\":%u,\"height\":%u,"
                  "\"fps\":%u,\"status\":\"%s\",\"recovered\":%s,"
                  "\"schema_version\":1}",
                  info.recording_id,
                  info.category,
                  static_cast<unsigned int>(info.start_time),
                  static_cast<unsigned int>(info.duration_s),
                  static_cast<unsigned int>(info.frame_count),
                  static_cast<unsigned int>(info.size_bytes),
                  static_cast<unsigned int>(info.width),
                  static_cast<unsigned int>(info.height),
                  static_cast<unsigned int>(info.fps),
                  info.status,
                  info.recovered ? "true" : "false");
    return httpReplyOk(req, rid, payload);
}

// --- GET /recordings/{id}/download --- download AVI bytes
static esp_err_t recordingDownloadHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    char qs[128] = {};
    size_t qs_size = sizeof(qs);
    if (httpd_req_get_url_query_str(req, qs, qs_size) != ESP_OK)
        qs[0] = '\0';
    char rec_id[32] = {};
    httpd_query_key_value(qs, "id", rec_id, sizeof(rec_id));
    if (rec_id[0] == '\0') {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "recording id required");
    }
    RecordingInfo info{};
    if (visioncam::recordingManagerLookup(rec_id, &info) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::NotFound,
                              "recording not found");
    }
    uint32_t fsize = info.size_bytes;
    if (fsize == 0 || fsize > 8 * 1024 * 1024) fsize = 8 * 1024 * 1024;
    uint8_t* fbuf = static_cast<uint8_t*>(malloc(fsize));
    if (fbuf == nullptr) {
        return httpReplyError(req, rid, ErrCode::Internal, "no memory");
    }
    size_t got = 0;
    esp_err_t rerr = visioncam::storageReadFile(info.path, fbuf, fsize, &got);
    if (rerr != ESP_OK || got == 0) {
        free(fbuf);
        return httpReplyError(req, rid, ErrCode::NotFound,
                              "recording file not found");
    }
    httpd_resp_set_type(req, "video/x-msvideo");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       std::string("attachment; filename=\"")
                           .append(info.recording_id)
                           .append(".avi\"")
                           .c_str());
    size_t sent = 0;
    while (sent < got) {
        const size_t chunk = std::min(got - sent, static_cast<size_t>(2048));
        if (httpd_resp_send_chunk(req,
                                  reinterpret_cast<const char*>(fbuf + sent),
                                  chunk) != ESP_OK) {
            free(fbuf);
            return ESP_FAIL;
        }
        sent += chunk;
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    free(fbuf);
    return ESP_OK;
}

// --- GET /recordings/{id}/play --- server-side MJPEG replay
static esp_err_t recordingPlayHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    char qs[128] = {};
    size_t qs_size = sizeof(qs);
    if (httpd_req_get_url_query_str(req, qs, qs_size) != ESP_OK)
        qs[0] = '\0';
    char rec_id[32] = {};
    httpd_query_key_value(qs, "id", rec_id, sizeof(rec_id));
    if (rec_id[0] == '\0') {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "recording id required");
    }
    // Check if playback can start (throughput share, etc.):
    PlaybackResult pr = visioncam::playbackCanStart(rec_id);
    if (pr == PlaybackResult::Busy) {
        return httpReplyError(req, rid, ErrCode::PlaybackBusy,
                              "playback busy - recording has priority");
    }
    if (pr != PlaybackResult::Ok) {
        return httpReplyError(req, rid, ErrCode::NotFound,
                              "recording not available for playback");
    }
    uint32_t start_frame = 0, start_ms = 0;
    char val[16] = {};
    if (httpd_query_key_value(qs, "start_frame", val, sizeof(val)) == ESP_OK)
        start_frame = static_cast<uint32_t>(std::atoi(val));
    val[0] = '\0';
    if (httpd_query_key_value(qs, "start_ms", val, sizeof(val)) == ESP_OK)
        start_ms = static_cast<uint32_t>(std::atoi(val));

    esp_err_t err = visioncam::playbackStreamStart(req, rec_id,
                                                   start_frame, start_ms);
    if (err != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "playback start failed");
    }
    // Stream frames until done:
    while (true) {
        PlaybackResult result = visioncam::playbackStreamFrame(req);
        if (result == PlaybackResult::Stopped) break;
        if (result != PlaybackResult::Ok) {
            visioncam::playbackStreamEnd(req);
            return httpReplyError(req, rid, ErrCode::Internal,
                                  "playback stream failed");
        }
    }
    visioncam::playbackStreamEnd(req);
    return ESP_OK;
}

// --- DELETE /recordings/{id} ---
static esp_err_t recordingDeleteHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (requireCsrf(req) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                              "CSRF token missing or invalid");
    }
    char qs[128] = {};
    size_t qs_size = sizeof(qs);
    if (httpd_req_get_url_query_str(req, qs, qs_size) != ESP_OK)
        qs[0] = '\0';
    char rec_id[32] = {};
    httpd_query_key_value(qs, "id", rec_id, sizeof(rec_id));
    if (rec_id[0] == '\0') {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "recording id required");
    }
    esp_err_t err = visioncam::recordingManagerDelete(rec_id);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpReplyError(req, rid, ErrCode::NotFound,
                              "recording not found");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "recording is active, stop first");
    }
    if (err != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "recording delete failed");
    }
    return httpReplyOk(req, rid, "{\"deleted\":true}");
}

// --- GET /motion/config --- (Phase 4 task 4.4)
static esp_err_t motionConfigGetHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    MotionConfig cfg{};
    if (visioncam::motionManagerGetConfig(&cfg) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "motion config read failed");
    }
    char payload[256] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"enabled\":%s,\"sample_rate_idle_hz\":%u,"
                  "\"sample_rate_rec_hz\":%u,\"threshold\":%u,"
                  "\"min_changed_ratio\":%u,\"cooldown_ms\":%u,"
                  "\"min_frames\":%u,\"settle_frames\":%u,"
                  "\"record_on_motion\":%s,\"snapshot_on_motion\":%s,"
                  "\"mqtt_notify\":%s,\"schema_version\":1}",
                  cfg.enabled ? "true" : "false",
                  static_cast<unsigned int>(cfg.sample_rate_idle_hz),
                  static_cast<unsigned int>(cfg.sample_rate_rec_hz),
                  static_cast<unsigned int>(cfg.threshold),
                  static_cast<unsigned int>(cfg.min_changed_ratio),
                  static_cast<unsigned int>(cfg.cooldown_ms),
                  static_cast<unsigned int>(cfg.min_frames),
                  static_cast<unsigned int>(cfg.settle_frames),
                  cfg.record_on_motion ? "true" : "false",
                  cfg.snapshot_on_motion ? "true" : "false",
                  cfg.mqtt_notify ? "true" : "false");
    return httpReplyOk(req, rid, payload);
}

// --- PUT /motion/config --- (Phase 4 task 4.4; SPEC 7 update process)
static esp_err_t motionConfigPutHandler(httpd_req_t* req) {
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
    MotionConfig cur{};
    if (visioncam::motionManagerGetConfig(&cur) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "motion config read failed");
    }
    MotionConfig cand = cur;
    char val[16] = {};
    bool any = false;
    if (jsonScalar(body, "\"enabled\"", val, sizeof(val))) {
        cand.enabled = (std::strcmp(val, "true") == 0 ||
                        std::strcmp(val, "1") == 0);
        any = true;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"sample_rate_idle_hz\"", val, sizeof(val))) {
        uint32_t v = 0;
        if (!parseU32Strict(val, v) || v > 100) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "sample_rate_idle_hz must be 1..100");
        }
        cand.sample_rate_idle_hz = v;
        any = true;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"sample_rate_rec_hz\"", val, sizeof(val))) {
        uint32_t v = 0;
        if (!parseU32Strict(val, v) || v > 100) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "sample_rate_rec_hz must be 1..100");
        }
        cand.sample_rate_rec_hz = v;
        any = true;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"threshold\"", val, sizeof(val))) {
        uint32_t v = 0;
        if (!parseU32Strict(val, v) || v > 255) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "threshold must be 0..255");
        }
        cand.threshold = v;
        any = true;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"min_changed_ratio\"", val, sizeof(val))) {
        uint32_t v = 0;
        if (!parseU32Strict(val, v) || v > 100) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "min_changed_ratio must be 0..100");
        }
        cand.min_changed_ratio = v;
        any = true;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"cooldown_ms\"", val, sizeof(val))) {
        uint32_t v = 0;
        if (!parseU32Strict(val, v) || v > 60000) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "cooldown_ms must be 0..60000");
        }
        cand.cooldown_ms = v;
        any = true;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"min_frames\"", val, sizeof(val))) {
        uint32_t v = 0;
        if (!parseU32Strict(val, v) || v > 100) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "min_frames must be 1..100");
        }
        cand.min_frames = v;
        any = true;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"settle_frames\"", val, sizeof(val))) {
        uint32_t v = 0;
        if (!parseU32Strict(val, v) || v > 50) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "settle_frames must be 1..50");
        }
        cand.settle_frames = v;
        any = true;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"record_on_motion\"", val, sizeof(val))) {
        cand.record_on_motion = (std::strcmp(val, "true") == 0 ||
                                 std::strcmp(val, "1") == 0);
        any = true;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"snapshot_on_motion\"", val, sizeof(val))) {
        cand.snapshot_on_motion = (std::strcmp(val, "true") == 0 ||
                                   std::strcmp(val, "1") == 0);
        any = true;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"mqtt_notify\"", val, sizeof(val))) {
        cand.mqtt_notify = (std::strcmp(val, "true") == 0 ||
                            std::strcmp(val, "1") == 0);
        any = true;
    }
    if (!any) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "no motion fields in body");
    }
    MotionConfig effective{};
    if (visioncam::motionManagerSetConfig(&cand, &effective) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "motion config write failed");
    }
    char payload[256] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"enabled\":%s,\"sample_rate_idle_hz\":%u,"
                  "\"sample_rate_rec_hz\":%u,\"threshold\":%u,"
                  "\"min_changed_ratio\":%u,\"cooldown_ms\":%u,"
                  "\"min_frames\":%u,\"settle_frames\":%u,"
                  "\"record_on_motion\":%s,\"snapshot_on_motion\":%s,"
                  "\"mqtt_notify\":%s,\"schema_version\":1}",
                  effective.enabled ? "true" : "false",
                  static_cast<unsigned int>(effective.sample_rate_idle_hz),
                  static_cast<unsigned int>(effective.sample_rate_rec_hz),
                  static_cast<unsigned int>(effective.threshold),
                  static_cast<unsigned int>(effective.min_changed_ratio),
                  static_cast<unsigned int>(effective.cooldown_ms),
                  static_cast<unsigned int>(effective.min_frames),
                  static_cast<unsigned int>(effective.settle_frames),
                  effective.record_on_motion ? "true" : "false",
                  effective.snapshot_on_motion ? "true" : "false",
                  effective.mqtt_notify ? "true" : "false");
    return httpReplyOk(req, rid, payload);
}

// --- GET /motion/status --- (Phase 4 task 4.4)
static esp_err_t motionStatusHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    MotionStatus st{};
    if (visioncam::motionManagerStatus(&st) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "motion status failed");
    }
    char payload[256] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"enabled\":%s,\"active\":%s,"
                  "\"events_total\":%u,\"events_last_ts_ms\":%u,"
                  "\"checks_total\":%u,\"checks_skipped\":%u,"
                  "\"changed_ratio_last\":%u,\"light_level_last\":%u,"
                  "\"decode_time_ms_last\":%u,\"analyze_time_ms_last\":%u,"
                  "\"schema_version\":1}",
                  st.enabled ? "true" : "false",
                  st.active ? "true" : "false",
                  static_cast<unsigned int>(st.events_total),
                  static_cast<unsigned int>(st.events_last_ts_ms),
                  static_cast<unsigned int>(st.checks_total),
                  static_cast<unsigned int>(st.checks_skipped),
                  static_cast<unsigned int>(st.changed_ratio_last),
                  static_cast<unsigned int>(st.light_level_last),
                  static_cast<unsigned int>(st.decode_time_ms_last),
                  static_cast<unsigned int>(st.analyze_time_ms_last));
    return httpReplyOk(req, rid, payload);
}
// --- GET /camera/stream ----------------------------------------------
// Live MJPEG for the Live View page (mockup Live.png). Registered here;
// the endpoint simply never existed before — /camera/capture (a single
// frame) was the only way to see the camera, which is why the Phase 1
// "live view" claim could not actually be reproduced in a browser.
//
// Contract:
//   - auth required (SPEC §6.3): a stream is still media.
//   - the handler never touches esp_camera_fb_* — it takes leases from the
//     frame manager, copies nothing, and releases the lease on EVERY exit
//     path (AGENTS rule 9). A slow client loses frames (the consumer queue
//     is depth-2 drop-oldest by design) instead of stalling capture.
//   - bounded: kMaxLiveStreams concurrent streams, an idle timeout when
//     the camera produces nothing, and an overall session cap so a
//     forgotten browser tab cannot hold a socket/subscriber forever.
//   - the subscription is per connection and is always released (RAII).
constexpr int kMaxLiveStreams = 2;
constexpr uint32_t kLiveStreamIdleMs = 10000;   // no frame -> close
constexpr uint32_t kLiveStreamMaxMs = 30u * 60u * 1000u;  // 30 min cap
// Hard ceiling for one frame copy (AGENTS rule 13: fixed, never sized from
// user input). A frame larger than this is dropped, never truncated.
constexpr size_t kLiveStreamFrameCap = 256 * 1024;

// Concurrent live streams (bounded — a stream owns a socket AND a frame
// subscriber slot; the httpd task is single, so more streams only starve
// the APIs).
static std::atomic<int> g_live_streams{0};

// RAII frame-manager subscription for a live stream. The registry is
// bounded (VC_FRAME_SUBSCRIBERS) and the handler must give the slot back
// on every exit path — a leak would starve the snapshot capture.
struct LiveSubGuard {
    FrameSubscriberId sub = 0;
    bool ok = false;
    esp_err_t acquire() {
        const esp_err_t err = frameManagerSubscribe(&sub, "livestream");
        ok = (err == ESP_OK);
        return err;
    }
    ~LiveSubGuard() {
        if (ok) frameManagerUnsubscribe(sub);
    }
};

// Owns the per-connection frame copy. frame_manager.h is explicit: a
// consumer that cannot hold a lease across blocking I/O copies the frame
// with frameLeaseCopyTo() and releases the lease IMMEDIATELY. Sending
// lease.data after frameRelease() would be a use-after-free (the record
// goes back to the pool/buffers) — the copy is what makes a slow stream
// safe for capture (rule 9). Freed on every exit path; never larger than
// kLiveStreamFrameCap; oversized frames are dropped, never truncated.
struct LiveStreamBuffer {
    uint8_t* data = nullptr;
    size_t cap = 0;
    bool growTo(size_t need) {
        if (need <= cap) return true;
        if (need > kLiveStreamFrameCap) return false;
        uint8_t* p = static_cast<uint8_t*>(
            heap_caps_malloc(need, MALLOC_CAP_DEFAULT));
        if (p == nullptr) return false;
        heap_caps_free(data);
        data = p;
        cap = need;
        return true;
    }
    ~LiveStreamBuffer() { heap_caps_free(data); }
};

static esp_err_t cameraStreamHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (!cameraManagerInitialised()) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "camera not initialised");
    }
    // Bounded concurrency: beyond this the socket stays free for the
    // settings/media APIs. Reported with the envelope code, not a hang.
    if (g_live_streams.fetch_add(1) >= kMaxLiveStreams) {
        g_live_streams.fetch_sub(1);
        return httpReplyError(req, rid, ErrCode::StreamSlotLimit,
                              "too many live streams");
    }
    struct StreamCountGuard {
        ~StreamCountGuard() { g_live_streams.fetch_sub(1); }
    } count_guard;

    LiveSubGuard sub_guard;
    if (sub_guard.acquire() != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "stream slot unavailable");
    }

    httpd_resp_set_type(req, "multipart/x-mixed-replace");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Content-Type",
                       "multipart/x-mixed-replace; boundary=--vcframe");

    const uint32_t started =
        static_cast<uint32_t>(esp_timer_get_time() / 1000);
    LiveStreamBuffer buf;
    for (;;) {
        const uint32_t now =
            static_cast<uint32_t>(esp_timer_get_time() / 1000);
        if (now - started > kLiveStreamMaxMs) break;

        FrameLease lease{};
        // Idle timeout: if no frame arrives the camera is not producing
        // (down, failed, recovering) — close instead of holding both the
        // socket and the subscriber slot indefinitely.
        if (!frameAcquire(sub_guard.sub, lease, kLiveStreamIdleMs)) break;

        // Copy out of the lease, release, THEN send: the send is blocking
        // socket I/O and the lease must never be held across it.
        const size_t len = lease.len;
        const bool size_ok = (len > 0 && buf.growTo(len));
        if (size_ok) frameLeaseCopyTo(lease, buf.data, buf.cap);
        frameRelease(lease);
        if (!size_ok) continue;   // oversized/empty: dropped, not sent

        char hdr[128] = {};
        const int hlen = std::snprintf(hdr, sizeof(hdr),
                                       "--vcframe\r\n"
                                       "Content-Type: image/jpeg\r\n"
                                       "Content-Length: %u\r\n\r\n",
                                       static_cast<unsigned>(len));
        if (hlen <= 0) continue;
        if (httpd_resp_send_chunk(req, hdr, hlen) != ESP_OK) break;
        if (httpd_resp_send_chunk(req,
                                  reinterpret_cast<const char*>(buf.data),
                                  len) != ESP_OK) break;
    }
    // Normal end of a chunked multipart response; the return code is
    // irrelevant because the client is gone by definition.
    httpd_resp_send_chunk(req, nullptr, 0);
    ESP_LOGI(TAG, "live stream ended after %u ms",
             static_cast<unsigned>(
                 static_cast<uint32_t>(esp_timer_get_time() / 1000) -
                 started));
    return ESP_OK;
}

// --- POST /snapshot --- (plan 02 task 2.3)
// Capture a manual snapshot and save it to SD. Requires auth + CSRF.
static esp_err_t snapshotCaptureHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (requireCsrf(req) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                              "CSRF token missing or invalid");
    }
    if (!visioncam::snapshotManagerAvailable()) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "snapshot manager not available");
    }
    // Same gate the snapshot manager enforces (it refuses to write a
    // 1970/01/01 path when SNTP has not set the clock). Checked here too
    // so the client gets TimeUnsynchronised instead of a bare
    // "capture failed: ESP_ERR_INVALID_STATE".
    if (visioncam::wallClockSeconds() < visioncam::kWallClockEraThreshold) {
        return httpReplyError(req, rid, ErrCode::TimeUnsynchronised,
                              "device clock not set - waiting for time sync");
    }
    char media_id[32] = {};
    const esp_err_t err =
        visioncam::snapshotCaptureManual(media_id, sizeof(media_id));
    if (err != ESP_OK) {
        char msg[96] = {};
        std::snprintf(msg, sizeof(msg), "snapshot capture failed: %s",
                      esp_err_to_name(err));
        return httpReplyError(req, rid, ErrCode::Internal, msg);
    }
    if (media_id[0] == '\0') {
        // Suppressed by cooldown — not an error.
        return httpReplyOk(req, rid, "{\"suppressed\":true,\"reason\":\"cooldown\"}");
    }
    char payload[80] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"media_id\":\"%s\",\"captured\":true}", media_id);
    return httpReplyOk(req, rid, payload);
}

// --- GET /snapshot?action=image&id=med-... ---
// Serve image bytes for a media ID. Requires auth.
static esp_err_t snapshotImageHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    char qs[128] = {};
    size_t qs_size = sizeof(qs);
    if (httpd_req_get_url_query_str(req, qs, qs_size) != ESP_OK) qs[0] = '\0';
    char media_id[32] = {};
    httpd_query_key_value(qs, "id", media_id, sizeof(media_id));
    if (media_id[0] == '\0') {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "media id required");
    }
    visioncam::MediaRecord rec{};
    if (visioncam::storageLookupMedia(media_id, &rec) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::NotFound,
                              "media not found");
    }
    // Read the whole file via the storage manager (AGENTS rule 10) into a
    // bounded heap buffer, then stream it in chunks.
    uint32_t fsize = rec.size;
    if (fsize == 0 || fsize > 2 * 1024 * 1024) fsize = 2 * 1024 * 1024; // cap
    uint8_t* fbuf = static_cast<uint8_t*>(malloc(fsize));
    if (fbuf == nullptr) {
        return httpReplyError(req, rid, ErrCode::Internal, "no memory");
    }
    size_t got = 0;
    const esp_err_t rerr =
        visioncam::storageReadFile(rec.path, fbuf, fsize, &got);
    if (rerr != ESP_OK || got == 0) {
        free(fbuf);
        return httpReplyError(req, rid, ErrCode::NotFound,
                              "media file not found");
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    size_t sent = 0;
    while (sent < got) {
        const size_t chunk = std::min(got - sent, static_cast<size_t>(2048));
        if (httpd_resp_send_chunk(req, reinterpret_cast<const char*>(fbuf + sent),
                                  chunk) != ESP_OK) {
            free(fbuf);
            return ESP_FAIL;
        }
        sent += chunk;
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    free(fbuf);
    ESP_LOGI(TAG, "snapshot image sent: %s %u bytes", media_id,
             static_cast<unsigned>(sent));
    return ESP_OK;
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
    config.max_uri_handlers = 48;   // Phase 1-6 routes + catch-all
    config.lru_purge_enable = kHttpLruPurge;
    // The dispatcher's DEFAULT matcher is httpd_uri_match_simple(), an EXACT
    // length-and-prefix compare — a registered "/*" therefore never matches
    // anything (proven on hardware: GET / answered 404 "Nothing matches the
    // given URI", so the embedded React UI was never served and the SPA
    // fallback never fired). Assigning the wildcard matcher is the documented
    // way to make "*" work; exact URIs still match identically.
    // NOTE: with wildcards on, "/*" matches EVERY GET URI, so it must stay
    // the LAST entry in the route table — any handler registered after it
    // fails registration with HTTPD_ERR_HANDLER_EXISTS.
    config.uri_match_fn = httpd_uri_match_wildcard;
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

// --- GET /integrations/mqtt --- (SPEC 6.9; Phase 7 task 7.4)
// Config + live status. The password is NEVER echoed (AGENTS rule 17):
// the response carries password_set instead.
static esp_err_t mqttIntegrationGetHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);

    MqttConfig cfg{};
    visioncam::mqttManagerGetConfig(&cfg);
    MqttStatus st{};
    visioncam::mqttManagerStatus(&st);

    char payload[768] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"enabled\":%s,"
                  "\"broker_uri\":\"%s\",\"username\":\"%s\","
                  "\"password_set\":%s,\"tls_verify\":%s,"
                  "\"base_topic\":\"%s\",\"client_id\":\"%s\","
                  "\"ha_discovery\":%s,\"ha_prefix\":\"%s\","
                  "\"keepalive_s\":%u,\"timeout_ms\":%u,"
                  "\"status\":{\"connected\":%s,\"state\":\"%s\","
                  "\"connects\":%u,\"disconnects\":%u,"
                  "\"published\":%u,\"dropped\":%u,"
                  "\"last_error\":\"%s\",\"ha_discovery_published\":%s},"
                  "\"schema_version\":1}",
                  cfg.enabled ? "true" : "false",
                  cfg.broker_uri.c_str(), cfg.username.c_str(),
                  visioncam::mqttManagerPasswordSet() ? "true" : "false",
                  cfg.tls_verify ? "true" : "false",
                  cfg.base_topic.c_str(), cfg.client_id.c_str(),
                  cfg.ha_discovery ? "true" : "false",
                  cfg.ha_prefix.c_str(),
                  static_cast<unsigned>(cfg.keepalive_s),
                  static_cast<unsigned>(cfg.timeout_ms),
                  st.connected ? "true" : "false", st.state.c_str(),
                  static_cast<unsigned>(st.connects),
                  static_cast<unsigned>(st.disconnects),
                  static_cast<unsigned>(st.published),
                  static_cast<unsigned>(st.dropped),
                  st.last_error.c_str(),
                  st.ha_discovery_published ? "true" : "false");
    return httpReplyOk(req, rid, payload);
}

// --- PUT /integrations/mqtt --- (SPEC 6.9; SPEC 7 update process)
// Partial overlay: only keys present in the body change. Password is
// write-only — sending "" keeps the stored one.
static esp_err_t mqttIntegrationPutHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (requireCsrf(req) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                              "CSRF token missing or invalid");
    }
    char body[512] = {};
    size_t blen = 0;
    if (readJsonBody(req, body, sizeof(body), blen) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "invalid JSON body");
    }

    MqttConfig cur{};
    visioncam::mqttManagerGetConfig(&cur);   // password stripped
    MqttConfig cand = cur;
    // The GET copy never carries the password, so re-read it via status:
    // mqttManagerSetConfig treats an empty password as "keep stored".
    char val[192] = {};

    if (jsonScalar(body, "\"enabled\"", val, sizeof(val))) {
        cand.enabled = (std::strcmp(val, "true") == 0 ||
                        std::strcmp(val, "1") == 0);
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"broker_uri\"", val, sizeof(val))) {
        cand.broker_uri = val;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"username\"", val, sizeof(val))) {
        cand.username = val;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"password\"", val, sizeof(val))) {
        cand.password = val;   // write-only; "" = keep stored
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"tls_verify\"", val, sizeof(val))) {
        cand.tls_verify = (std::strcmp(val, "true") == 0 ||
                           std::strcmp(val, "1") == 0);
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"base_topic\"", val, sizeof(val))) {
        cand.base_topic = val;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"ha_discovery\"", val, sizeof(val))) {
        cand.ha_discovery = (std::strcmp(val, "true") == 0 ||
                             std::strcmp(val, "1") == 0);
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"ha_prefix\"", val, sizeof(val))) {
        cand.ha_prefix = val;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"keepalive_s\"", val, sizeof(val))) {
        uint32_t v = 0;
        if (!parseU32Strict(val, v) || v < 10 || v > 300) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "keepalive_s must be 10..300");
        }
        cand.keepalive_s = v;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"timeout_ms\"", val, sizeof(val))) {
        uint32_t v = 0;
        if (!parseU32Strict(val, v) || v < 1000 || v > 60000) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "timeout_ms must be 1000..60000");
        }
        cand.timeout_ms = v;
    }

    MqttConfig effective{};
    const esp_err_t err = visioncam::mqttManagerSetConfig(&cand, &effective);
    if (err == ESP_ERR_INVALID_ARG) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "invalid MQTT config (check broker_uri scheme: "
                              "mqtt:// or mqtts://, and enable needs a URI)");
    }
    if (err != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "MQTT config persist failed");
    }
    char payload[640] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"enabled\":%s,\"broker_uri\":\"%s\","
                  "\"username\":\"%s\",\"password_set\":%s,"
                  "\"tls_verify\":%s,\"base_topic\":\"%s\","
                  "\"ha_discovery\":%s,\"ha_prefix\":\"%s\","
                  "\"keepalive_s\":%u,\"timeout_ms\":%u,"
                  "\"schema_version\":1}",
                  effective.enabled ? "true" : "false",
                  effective.broker_uri.c_str(), effective.username.c_str(),
                  visioncam::mqttManagerPasswordSet() ? "true" : "false",
                  effective.tls_verify ? "true" : "false",
                  effective.base_topic.c_str(),
                  effective.ha_discovery ? "true" : "false",
                  effective.ha_prefix.c_str(),
                  static_cast<unsigned>(effective.keepalive_s),
                  static_cast<unsigned>(effective.timeout_ms));
    return httpReplyOk(req, rid, payload);
}

// --- POST /integrations/mqtt/test --- (SPEC 6.9)
static esp_err_t mqttIntegrationTestHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (requireCsrf(req) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                              "CSRF token missing or invalid");
    }
    const esp_err_t err = visioncam::mqttManagerTestConnection();
    if (err == ESP_ERR_INVALID_STATE) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "no broker configured");
    }
    if (err == ESP_ERR_TIMEOUT) {
        return httpReplyError(req, rid, ErrCode::MqttUnavailable,
                              "broker not reachable within timeout");
    }
    if (err != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "connection test failed");
    }
    return httpReplyOk(req, rid, "{\"connected\":true}");
}

// --- POST /integrations/mqtt/reconnect --- (SPEC 6.9)
static esp_err_t mqttIntegrationReconnectHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (requireCsrf(req) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                              "CSRF token missing or invalid");
    }
    // Re-running the test tears down and rebuilds the client, which is
    // exactly a forced reconnect (the worker resumes its own loop after).
    const esp_err_t err = visioncam::mqttManagerTestConnection();
    if (err == ESP_ERR_INVALID_STATE) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "no broker configured");
    }
    return httpReplyOk(req, rid,
                       err == ESP_OK ? "{\"reconnected\":true}"
                                     : "{\"reconnected\":false,"
                                       "\"note\":\"broker not reachable\"}");
}

// --- POST /integrations/mqtt/test-publish --- (SPEC 6.9)
// Publishes one retained test message on the device's own topic tree and
// reports the queue outcome. No secrets, no image data.
static esp_err_t mqttIntegrationTestPublishHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (requireCsrf(req) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                              "CSRF token missing or invalid");
    }
    MqttStatus st{};
    visioncam::mqttManagerStatus(&st);
    if (!st.connected) {
        return httpReplyError(req, rid, ErrCode::MqttUnavailable,
                              "not connected to broker");
    }
    if (visioncam::mqttManagerPublishTest() != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "test publish failed");
    }
    return httpReplyOk(req, rid, "{\"published\":true}");
}

// --- POST /integrations/home-assistant/discovery --- (SPEC 6.9)
static esp_err_t haDiscoveryRepublishHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (requireCsrf(req) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                              "CSRF token missing or invalid");
    }
    const esp_err_t err = visioncam::mqttManagerRepublishDiscovery();
    if (err == ESP_ERR_INVALID_STATE) {
        return httpReplyError(req, rid, ErrCode::MqttUnavailable,
                              "MQTT not connected or discovery disabled");
    }
    if (err != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "discovery republish failed");
    }
    return httpReplyOk(req, rid, "{\"republished\":true}");
}

// --- GET /integrations/home-assistant/status --- (SPEC 6.9)
static esp_err_t haDiscoveryStatusHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    MqttConfig cfg{};
    visioncam::mqttManagerGetConfig(&cfg);
    MqttStatus st{};
    visioncam::mqttManagerStatus(&st);
    char payload[320] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"enabled\":%s,\"connected\":%s,"
                  "\"discovery_published\":%s,\"discovery_prefix\":\"%s\","
                  "\"state\":\"%s\",\"schema_version\":1}",
                  cfg.ha_discovery ? "true" : "false",
                  st.connected ? "true" : "false",
                  st.ha_discovery_published ? "true" : "false",
                  cfg.ha_prefix.c_str(), st.state.c_str());
    return httpReplyOk(req, rid, payload);
}

// --- GET /integrations/network-storage --- (SPEC 6.9; Phase 8 task 8.2)
// Returns config (secrets stripped) + live status.
static esp_err_t netStorageGetHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);

    auto* ns = visioncam::storageGetNetstoreBackend();
    NetworkStorageConfig cfg{};
    NetworkStorageStatus st{};
    if (ns != nullptr) {
        ns->getConfig(&cfg);
        ns->getStatus(&st);
    }
    char payload[2048] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"enabled\":%s,\"configured\":%s,\"available\":%s,"
                  "\"backend_type\":\"%s\",\"server_url\":\"%s\","
                  "\"destination_path\":\"%s\",\"upload_policy\":\"%s\","
                  "\"auth_token_configured\":%s,\"tls_verify\":%s,"
                  "\"retry_count\":%u,\"retry_interval_ms\":%u,"
                  "\"upload_queue_limit\":%u,\"delete_after_upload\":%s,"
                  "\"queue_depth\":%u,\"queue_max_seen\":%u,"
                  "\"uploads_total\":%u,\"uploads_ok\":%u,\"uploads_failed\":%u,"
                  "\"queue_dropped\":%u,\"last_error\":\"%s\",\"state\":\"%s\","
                  "\"schema_version\":1}",
                  cfg.enabled ? "true" : "false",
                  cfg.server_url[0] != '\0' ? "true" : "false",
                  st.available ? "true" : "false",
                  cfg.backend_type, cfg.server_url, cfg.destination_path,
                  cfg.upload_policy,
                  cfg.auth_token_configured ? "true" : "false",
                  cfg.tls_verify ? "true" : "false",
                  static_cast<unsigned>(cfg.retry_count),
                  static_cast<unsigned>(cfg.retry_interval_ms),
                  static_cast<unsigned>(cfg.upload_queue_limit),
                  cfg.delete_after_upload ? "true" : "false",
                  static_cast<unsigned>(st.queue_depth),
                  static_cast<unsigned>(st.queue_max_seen),
                  static_cast<unsigned>(st.uploads_total),
                  static_cast<unsigned>(st.uploads_ok),
                  static_cast<unsigned>(st.uploads_failed),
                  static_cast<unsigned>(st.queue_dropped),
                  st.last_error, st.state);
    return httpReplyOk(req, rid, payload);
}

// --- PUT /integrations/network-storage --- (SPEC 6.9; SPEC 7 update process)
// Partial overlay: only fields present in the body change. Auth token is
// write-only — sending "" keeps the stored value; GET never returns it.
static esp_err_t netStoragePutHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (requireCsrf(req) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                              "CSRF token missing or invalid");
    }
    auto* ns = visioncam::storageGetNetstoreBackend();
    if (ns == nullptr) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "network storage backend not initialised");
    }
    NetworkStorageConfig cur{};
    ns->getConfig(&cur);
    NetworkStorageConfig cand = cur;
    char body[512] = {};
    size_t blen = 0;
    if (readJsonBody(req, body, sizeof(body), blen) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "invalid JSON body");
    }
    char val[256] = {};

    if (jsonScalar(body, "\"enabled\"", val, sizeof(val))) {
        cand.enabled = (std::strcmp(val, "true") == 0 ||
                        std::strcmp(val, "1") == 0);
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"backend_type\"", val, sizeof(val))) {
        std::strncpy(cand.backend_type, val, sizeof(cand.backend_type) - 1);
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"server_url\"", val, sizeof(val))) {
        std::strncpy(cand.server_url, val, sizeof(cand.server_url) - 1);
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"destination_path\"", val, sizeof(val))) {
        std::strncpy(cand.destination_path, val,
                     sizeof(cand.destination_path) - 1);
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"auth_token\"", val, sizeof(val))) {
        std::strncpy(cand.auth_token, val, sizeof(cand.auth_token) - 1);
        // Non-empty token marks it as configured.
        if (val[0] != '\0') cand.auth_token_configured = true;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"upload_policy\"", val, sizeof(val))) {
        std::strncpy(cand.upload_policy, val, sizeof(cand.upload_policy) - 1);
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"retry_count\"", val, sizeof(val))) {
        uint32_t v = 0;
        if (!parseU32Strict(val, v) || v > 10) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "retry_count must be 0..10");
        }
        cand.retry_count = v;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"retry_interval_ms\"", val, sizeof(val))) {
        uint32_t v = 0;
        if (!parseU32Strict(val, v) || v < 1000 || v > 60000) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "retry_interval_ms must be 1000..60000");
        }
        cand.retry_interval_ms = v;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"tls_verify\"", val, sizeof(val))) {
        cand.tls_verify = (std::strcmp(val, "true") == 0 ||
                           std::strcmp(val, "1") == 0);
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"upload_queue_limit\"", val, sizeof(val))) {
        uint32_t v = 0;
        if (!parseU32Strict(val, v) || v > 128) {
            return httpReplyError(req, rid, ErrCode::ValidationFailed,
                                  "upload_queue_limit must be 1..128");
        }
        cand.upload_queue_limit = v;
    }
    val[0] = '\0';
    if (jsonScalar(body, "\"delete_after_upload\"", val, sizeof(val))) {
        cand.delete_after_upload =
            (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
    }

    NetworkStorageConfig effective{};
    const esp_err_t err = ns->setConfig(cand, &effective);
    if (err == ESP_ERR_INVALID_ARG) {
        return httpReplyError(req, rid, ErrCode::ValidationFailed,
                              "invalid network-storage config");
    }
    if (err != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "network-storage config persist failed");
    }
    char payload[1024] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"enabled\":%s,\"backend_type\":\"%s\",\"server_url\":\"%s\","
                  "\"destination_path\":\"%s\",\"upload_policy\":\"%s\","
                  "\"auth_token_configured\":%s,\"tls_verify\":%s,"
                  "\"retry_count\":%u,\"retry_interval_ms\":%u,"
                  "\"upload_queue_limit\":%u,\"delete_after_upload\":%s,"
                  "\"schema_version\":1}",
                  effective.enabled ? "true" : "false",
                  effective.backend_type, effective.server_url,
                  effective.destination_path, effective.upload_policy,
                  effective.auth_token_configured ? "true" : "false",
                  effective.tls_verify ? "true" : "false",
                  static_cast<unsigned>(effective.retry_count),
                  static_cast<unsigned>(effective.retry_interval_ms),
                  static_cast<unsigned>(effective.upload_queue_limit),
                  effective.delete_after_upload ? "true" : "false");
    return httpReplyOk(req, rid, payload);
}

// --- POST /integrations/network-storage/test --- (SPEC 6.9)
// Reachability probe: a real GET to the configured server (not a saved
// setting). Reports the actual HTTP status.
static esp_err_t netStorageTestHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    if (requireCsrf(req) != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::CsrfInvalid,
                              "CSRF token missing or invalid");
    }
    auto* ns = visioncam::storageGetNetstoreBackend();
    if (ns == nullptr) {
        return httpReplyError(req, rid, ErrCode::Internal,
                              "network storage backend not initialised");
    }
    const esp_err_t err = ns->testConnection();
    if (err != ESP_OK) {
        return httpReplyError(req, rid, ErrCode::NetworkStorageUnavailable,
                              "backend unreachable or heap gate closed");
    }
    return httpReplyOk(req, rid, "{\"reachable\":true}");
}

// --- GET /integrations/network-storage/queue --- (SPEC 6.9)
// Upload queue status: depth, high-water, dropped counts.
static esp_err_t netStorageQueueHandler(httpd_req_t* req) {
    uint32_t rid = httpNextRequestId();
    if (requireAuth(req) != ESP_OK) return replyUnauthenticated(req, rid);
    auto* ns = visioncam::storageGetNetstoreBackend();
    NetworkStorageStatus st{};
    if (ns != nullptr) ns->getStatus(&st);
    char payload[320] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"queue_depth\":%u,\"queue_max_seen\":%u,"
                  "\"uploads_total\":%u,\"uploads_ok\":%u,"
                  "\"uploads_failed\":%u,\"queue_dropped\":%u,"
                  "\"state\":\"%s\",\"schema_version\":1}",
                  static_cast<unsigned>(st.queue_depth),
                  static_cast<unsigned>(st.queue_max_seen),
                  static_cast<unsigned>(st.uploads_total),
                  static_cast<unsigned>(st.uploads_ok),
                  static_cast<unsigned>(st.uploads_failed),
                  static_cast<unsigned>(st.queue_dropped),
                  st.state);
    return httpReplyOk(req, rid, payload);
}

// --- route table ------------------------------------------------------------
static const httpd_uri_t s_routes[] = {
    // Auth
    {"/auth/session", HTTP_GET, authSessionHandler, nullptr},
    {"/auth/login", HTTP_POST, authLoginHandler, nullptr},
    {"/auth/logout", HTTP_POST, authLogoutHandler, nullptr},
    {"/auth/complete-first-login", HTTP_POST, authCompleteFirstLoginHandler,
     nullptr},
    {"/auth/change-password", HTTP_POST, authChangePasswordHandler, nullptr},
    // Wi-Fi provisioning, available only while the device is in setup AP mode.
    {"/wifi/config", HTTP_POST, wifiConfigPostHandler, nullptr},
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
    // Live MJPEG view (Phase 1 exit gate: authenticated live stream).
    {"/camera/stream", HTTP_GET, cameraStreamHandler, nullptr},    // Snapshot (plan 02 task 2.3)
    {"/snapshot", HTTP_POST, snapshotCaptureHandler, nullptr},
    {"/snapshot", HTTP_GET, snapshotImageHandler, nullptr},
    // Storage
    {"/storage", HTTP_GET, storageHandler, nullptr},
    {"/storage", HTTP_DELETE, storageHandler, nullptr},
    {"/storage", HTTP_POST, storageHandler, nullptr},
    {"/storage", HTTP_PUT, storageHandler, nullptr},
    // Recording (Phase 3)
    {"/recording/status", HTTP_GET, recordingStatusHandler, nullptr},
    {"/recording/config", HTTP_GET, recordingConfigGetHandler, nullptr},
    {"/recording/config", HTTP_PUT, recordingConfigPutHandler, nullptr},
    {"/recording/start", HTTP_POST, recordingStartHandler, nullptr},
    {"/recording/stop", HTTP_POST, recordingStopHandler, nullptr},
    {"/recordings", HTTP_GET, recordingsListHandler, nullptr},
    {"/recordings", HTTP_DELETE, recordingDeleteHandler, nullptr},
    // Motion (Phase 4)
    {"/motion/config", HTTP_GET, motionConfigGetHandler, nullptr},
    {"/motion/config", HTTP_PUT, motionConfigPutHandler, nullptr},
    {"/motion/status", HTTP_GET, motionStatusHandler, nullptr},
    // Integrations (Phase 7, SPEC 6.9)
    {"/integrations/mqtt", HTTP_GET, mqttIntegrationGetHandler, nullptr},
    {"/integrations/mqtt", HTTP_PUT, mqttIntegrationPutHandler, nullptr},
    {"/integrations/mqtt/test", HTTP_POST, mqttIntegrationTestHandler,
     nullptr},
    {"/integrations/mqtt/reconnect", HTTP_POST,
     mqttIntegrationReconnectHandler, nullptr},
    {"/integrations/mqtt/test-publish", HTTP_POST,
     mqttIntegrationTestPublishHandler, nullptr},
    {"/integrations/home-assistant/discovery", HTTP_POST,
     haDiscoveryRepublishHandler, nullptr},
    {"/integrations/home-assistant/status", HTTP_GET,
     haDiscoveryStatusHandler, nullptr},
    // Integrations (Phase 8, SPEC 6.9)
    {"/integrations/network-storage", HTTP_GET, netStorageGetHandler,
     nullptr},
    {"/integrations/network-storage", HTTP_PUT, netStoragePutHandler,
     nullptr},
    {"/integrations/network-storage/test", HTTP_POST,
     netStorageTestHandler, nullptr},
    {"/integrations/network-storage/queue", HTTP_GET,
     netStorageQueueHandler, nullptr},
    // Timelapse (Phase 5) — descoped, routes intentionally absent
    // AI (Phase 6) — descoped, routes intentionally absent
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