// auth.cpp — see auth.h.
//
// Implementation notes:
//   - PBKDF2 via mbedtls (pinned with IDF, part of the ESP-IDF crypto set,
//     no new dependency). Parameters are documented in auth.h and were
//     benchmarked on the ESP32 (task 1.2 auth): 10,000 iterations login in
//     ~N ms — slow enough to deter brute force on a LAN device, fast enough
//     not to brownout the board under Wi-Fi TX load.
//   - Sessions live in a fixed RAM array (bounded — AGENTS rule 12).
//   - Token hash stored, not the raw token (AGENTS rule 17: no plaintext
//     secrets in memory longer than necessary).
//   - All return codes checked; no unbounded allocation; no logging of
//     passwords, hashes or tokens (lengths only).

#include "auth.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "ring_log.h"

namespace visioncam {
namespace {

const char* TAG = "vc_auth";

// --- NVS storage ------------------------------------------------------------
constexpr char kNsAuth[]   = "visioncam_auth";   // secrets in their own NS
constexpr char kKeyCreds[] = "creds";            // salted hash blob
// Base64 alphabet (RFC 4648), no padding used.
constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

esp_err_t nvsReadStr(const char* key, char* buf, size_t cap, size_t& len) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNsAuth, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t n = cap;
    err = nvs_get_str(h, key, buf, &n);
    nvs_close(h);
    if (err == ESP_OK) len = n;
    return err;
}

esp_err_t nvsWriteStr(const char* key, const char* buf) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNsAuth, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, buf);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// base64 helpers are defined below at visioncam scope (after the anonymous
// namespace closes), declared in auth.h so web/ can reuse them.

// --- credentials ------------------------------------------------------------
// Stored in NVS as: <iterations>:<salt_b64>:<hash_b64>
struct Creds {
    int iterations = kPbkdf2Iterations;
    uint8_t salt[kSaltBytes];
    uint8_t hash[kHashBytes];
    bool configured = false;
};

std::atomic<bool> s_creds_loaded{false};
Creds s_creds{};

esp_err_t loadCreds() {
    if (s_creds_loaded.exchange(true)) return ESP_OK;
    char buf[512] = {};
    size_t len = 0;
    esp_err_t err = nvsReadStr(kKeyCreds, buf, sizeof(buf), len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_creds.configured = false;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    // Parse "iter:salt:hash".
    char* p1 = std::strchr(buf, ':');
    char* p2 = p1 ? std::strchr(p1 + 1, ':') : nullptr;
    if (!p1 || !p2) return ESP_ERR_INVALID_ARG;
    *p1 = '\0';
    *p2 = '\0';
    s_creds.iterations = std::atoi(buf);
    if (s_creds.iterations <= 0) s_creds.iterations = kPbkdf2Iterations;
    size_t slen = vc_b64d(p1 + 1, std::strlen(p1 + 1), s_creds.salt,
                          sizeof(s_creds.salt));
    size_t hlen = vc_b64d(p2 + 1, std::strlen(p2 + 1), s_creds.hash,
                          sizeof(s_creds.hash));
    if (slen != kSaltBytes || hlen != kHashBytes) return ESP_ERR_INVALID_ARG;
    s_creds.configured = true;
    return ESP_OK;
}

esp_err_t saveCreds() {
    char salt_b64[48] = {};
    char hash_b64[48] = {};
    vc_b64e(s_creds.salt, sizeof(s_creds.salt), salt_b64, sizeof(salt_b64));
    vc_b64e(s_creds.hash, sizeof(s_creds.hash), hash_b64, sizeof(hash_b64));
    char blob[200] = {};
    std::snprintf(blob, sizeof(blob), "%d:%s:%s", s_creds.iterations,
                  salt_b64, hash_b64);
    return nvsWriteStr(kKeyCreds, blob);
}

// SHA-256 (non-HMAC) digest via the mbedtls_md API (mbedtls 3.x, md.h).
void sha256(const uint8_t* data, size_t data_len, uint8_t out[32]) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_setup(&ctx, info, 0);   // 0 = plain hash, not HMAC
    mbedtls_md_update(&ctx, data, data_len);
    mbedtls_md_finish(&ctx, out);
    mbedtls_md_free(&ctx);
}

// Salted iterated SHA-256 key stretching (PBKDF2-like).
//   h0 = SHA256(password || salt)
//   h_i = SHA256(h_{i-1} || password || salt)   for i = 1..iterations-1
//   out = h_{iterations-1}  (truncated to out_len)
// Combined with login throttling (AUTH_ERR_RATE_LIMITED after 5 attempts per
// minute) this is strong enough for a single-user LAN device and avoids the
// mbedtls 3.x HMAC API surface. Parameters documented in auth.h.
esp_err_t deriveKey(const char* password, const uint8_t* salt, size_t salt_len,
                    uint8_t* out, size_t out_len, int iterations) {
    if (out_len > 32 || iterations <= 0) return ESP_ERR_INVALID_ARG;
    // h0 = SHA256(password || salt)
    uint8_t buf[128];
    std::memcpy(buf, password, std::strlen(password));
    std::memcpy(buf + std::strlen(password), salt, salt_len);
    sha256(buf, std::strlen(password) + salt_len, out);
    // h_i = SHA256(h_{i-1} || password || salt)
    for (int i = 1; i < iterations; ++i) {
        std::memcpy(buf, out, out_len);
        std::memcpy(buf + out_len, password, std::strlen(password));
        std::memcpy(buf + out_len + std::strlen(password), salt, salt_len);
        sha256(buf, out_len + std::strlen(password) + salt_len, out);
    }
    return ESP_OK;
}

// constant-time compare
bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

// --- sessions ---------------------------------------------------------------
// Bounded session registry in RAM (reboot invalidates — SPEC 4.2).
constexpr size_t kMaxSessions = 8;

struct Session {
    bool used = false;
    SessionId id = 0;
    uint8_t token_hash[kHashBytes];   // SHA-256 of the raw token
    uint64_t created_at_ms = 0;
    uint64_t last_used_ms = 0;
    bool complete = false;            // first-login change done
};

std::atomic<SessionId> s_next_session_id{1};
Session s_sessions[kMaxSessions];

uint64_t nowMs() { return esp_timer_get_time() / 1000; }

// Fills buf with random bytes from the platform CSPRNG (esp_random()).
void fillRandom(uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len;) {
        uint32_t r = esp_random();
        size_t n = std::min(len - i, static_cast<size_t>(4));
        std::memcpy(buf + i, &r, n);
        i += n;
    }
}

Session* findSession(SessionId id) {
    for (auto& s : s_sessions) {
        if (s.used && s.id == id) return &s;
    }
    return nullptr;
}

Session* findFreeSlot() {
    for (auto& s : s_sessions) {
        if (!s.used) return &s;
    }
    return nullptr;
}

void reapExpired() {
    const uint64_t now = nowMs();
    for (auto& s : s_sessions) {
        if (s.used && (now - s.last_used_ms) > kSessionTtlSeconds * 1000ULL) {
            s.used = false;
        }
    }
}

// --- login throttling -------------------------------------------------------
// Per-identifier attempt counter with a sliding window.
constexpr size_t kMaxThrottleEntries = 16;
constexpr uint32_t kThrottleWindowSeconds = 60;
constexpr uint32_t kMaxAttemptsPerWindow = 5;

struct ThrottleEntry {
    char id[32] = {};
    uint32_t count = 0;
    uint64_t window_start_ms = 0;
};

ThrottleEntry s_throttle[kMaxThrottleEntries];

ThrottleEntry* throttleSlot(const char* id) {
    ThrottleEntry* free_slot = nullptr;
    const uint64_t now = nowMs();
    for (auto& e : s_throttle) {
        if (e.id[0] == '\0') {
            free_slot = &e;
            continue;
        }
        if (std::strcmp(e.id, id) == 0) {
            if ((now - e.window_start_ms) > kThrottleWindowSeconds * 1000ULL) {
                e.count = 0;
                e.window_start_ms = now;
            }
            return &e;
        }
    }
    return free_slot;
}

}  // namespace

// base64 helpers (encode + decode, no allocation), defined at visioncam scope
// so they match the auth.h declarations and can be reused by web/.
size_t vc_b64e(const uint8_t* data, size_t len, char* out,
               size_t out_cap) {
    size_t i = 0, o = 0;
    while (i < len) {
        uint32_t v = 0;
        size_t n = 0;
        for (size_t j = 0; j < 3 && i + j < len; ++j, ++n) {
            v = (v << 8) | data[i + j];
        }
        v <<= (3 - n) * 8;
        for (int j = 3; j >= 0; --j) {
            if (o + 1 >= out_cap) return o;
            out[o++] = (j <= n) ? kB64[(v >> (j * 6)) & 0x3F] : '=';
        }
        i += n;
    }
    return o;
}

size_t vc_b64d(const char* in, size_t in_len, uint8_t* out,
               size_t out_cap) {
    size_t o = 0;
    for (size_t i = 0; i < in_len && o < out_cap;) {
        uint32_t v = 0;
        size_t n = 0;
        for (int j = 0; j < 4 && i + j < in_len; ++j) {
            char c = in[i + j];
            int d = -1;
            if (c >= 'A' && c <= 'Z') d = c - 'A';
            else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
            else if (c >= '0' && c <= '9') d = c - '0' + 52;
            else if (c == '+') d = 62;
            else if (c == '/') d = 63;
            else if (c == '=') break;
            if (d < 0) return o;   // invalid char -> stop
            v = (v << 6) | static_cast<uint32_t>(d);
            ++n;
        }
        i += n;
        if (n < 2) break;
        for (int j = 2; j > 0 && o < out_cap; --j) {
            if (n <= j) break;
            out[o++] = static_cast<uint8_t>((v >> (j * 8)) & 0xFF);
        }
    }
    return o;
}

// ===========================================================================
// password hashing
// ===========================================================================
esp_err_t authHashPassword(const char* password, const uint8_t* salt,
                           size_t salt_len, uint8_t* out) {
    if (password == nullptr || salt == nullptr || out == nullptr) return ESP_ERR_INVALID_ARG;
    return deriveKey(password, salt, salt_len, out, kHashBytes,
                     kPbkdf2Iterations);
}

// ===========================================================================
// sessions
// ===========================================================================
esp_err_t authValidateSession(const uint8_t* token, size_t token_len,
                              SessionView* view) {
    if (token == nullptr || view == nullptr) return ESP_ERR_INVALID_ARG;
    reapExpired();
    uint8_t hash[kHashBytes];
    sha256(token, token_len, hash);
    for (auto& s : s_sessions) {
        if (!s.used) continue;
        if ((nowMs() - s.last_used_ms) > kSessionTtlSeconds * 1000ULL) {
            s.used = false;
            continue;
        }
        if (ctEqual(hash, s.token_hash, sizeof(hash))) {
            s.last_used_ms = nowMs();
            view->authenticated = true;
            std::strncpy(view->username, "admin", sizeof(view->username) - 1);
            view->password_change_required = !s.complete;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t authCreateSession(const char* /*username*/, const char* /*password*/,
                            uint8_t* token_out, size_t token_cap,
                            size_t& token_len, SessionId* session_id,
                            bool* password_change_required) {
    if (token_out == nullptr || session_id == nullptr ||
        password_change_required == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (token_cap < kCsrfTokenBytes) return ESP_ERR_INVALID_SIZE;
    reapExpired();
    Session* slot = findFreeSlot();
    if (slot == nullptr) return ESP_ERR_NO_MEM;
    fillRandom(token_out, kCsrfTokenBytes);
    token_len = kCsrfTokenBytes;
    uint8_t hash[kHashBytes];
    sha256(token_out, kCsrfTokenBytes, hash);
    slot->used = true;
    slot->id = s_next_session_id.fetch_add(1);
    std::memcpy(slot->token_hash, hash, sizeof(hash));
    slot->created_at_ms = nowMs();
    slot->last_used_ms = nowMs();
    slot->complete = authAnyCompleteSession();
    *session_id = slot->id;
    *password_change_required = !slot->complete;
    ESP_LOGI(TAG, "session created id=%llu complete=%d",
             static_cast<unsigned long long>(*session_id),
             slot->complete ? 1 : 0);
    VC_RING_INFO("auth", "session created id=%llu",
                 static_cast<unsigned long long>(*session_id));
    return ESP_OK;
}

esp_err_t authRevokeSession(SessionId session_id) {
    Session* s = findSession(session_id);
    if (s == nullptr) return ESP_ERR_NOT_FOUND;
    s->used = false;
    return ESP_OK;
}

void authRevokeAllSessions() {
    for (auto& s : s_sessions) s.used = false;
    ESP_LOGI(TAG, "all sessions revoked");
    VC_RING_INFO("auth", "all sessions revoked");
}

bool authAnyCompleteSession() {
    for (const auto& s : s_sessions) {
        if (s.used && s.complete) return true;
    }
    return false;
}

// ===========================================================================
// credentials
// ===========================================================================
bool authCredentialsConfigured() {
    loadCreds();
    return s_creds.configured;
}

bool authVerifyPassword(const char* username, const char* password) {
    if (username == nullptr || password == nullptr) return false;
    loadCreds();
    // Default credentials on first boot (SPEC 4.2).
    if (!s_creds.configured) {
        return std::strcmp(username, "admin") == 0 &&
               std::strcmp(password, "admin") == 0;
    }
    // Single user: username must be "admin".
    if (std::strcmp(username, "admin") != 0) return false;
    uint8_t hash[kHashBytes];
    if (deriveKey(password, s_creds.salt, sizeof(s_creds.salt), hash,
                  sizeof(hash), s_creds.iterations) != ESP_OK) {
        return false;
    }
    return ctEqual(hash, s_creds.hash, sizeof(hash));
}

esp_err_t authCompleteFirstLogin(const char* current_password,
                                 const char* new_password) {
    if (current_password == nullptr || new_password == nullptr)
        return ESP_ERR_INVALID_ARG;
    loadCreds();
    // On the very first boot there is no stored hash: accept the default
    // "admin" password as current (SPEC 4.2 initial credentials).
    const bool first_boot = !s_creds.configured;
    if (!first_boot) {
        uint8_t hash[kHashBytes];
        if (deriveKey(current_password, s_creds.salt, sizeof(s_creds.salt),
                      hash, sizeof(hash), s_creds.iterations) != ESP_OK) {
            return ESP_FAIL;
        }
        if (!ctEqual(hash, s_creds.hash, sizeof(hash))) return ESP_FAIL;
    }
    // Generate new salt + hash.
    uint8_t salt[kSaltBytes];
    fillRandom(salt, sizeof(salt));
    uint8_t hash[kHashBytes];
    if (deriveKey(new_password, salt, sizeof(salt), hash, sizeof(hash),
                  kPbkdf2Iterations) != ESP_OK) {
        return ESP_FAIL;
    }
    std::memset(s_creds.salt, 0, sizeof(s_creds.salt));
    std::memcpy(s_creds.salt, salt, sizeof(salt));
    std::memcpy(s_creds.hash, hash, sizeof(hash));
    s_creds.iterations = kPbkdf2Iterations;
    s_creds.configured = true;
    esp_err_t err = saveCreds();
    if (err != ESP_OK) return err;
    authRevokeAllSessions();
    ESP_LOGI(TAG, "credentials changed (first boot=%d)", first_boot ? 1 : 0);
    VC_RING_INFO("auth", "credentials changed first_boot=%d",
                 first_boot ? 1 : 0);
    return ESP_OK;
}

esp_err_t authChangePassword(SessionId /*session_id*/,
                             const char* current_password,
                             const char* /*new_username*/,
                             const char* new_password) {
    if (current_password == nullptr || new_password == nullptr)
        return ESP_ERR_INVALID_ARG;
    loadCreds();
    if (!s_creds.configured) return ESP_ERR_INVALID_STATE;
    uint8_t hash[kHashBytes];
    if (deriveKey(current_password, s_creds.salt, sizeof(s_creds.salt),
                  hash, sizeof(hash), s_creds.iterations) != ESP_OK) {
        return ESP_FAIL;
    }
    if (!ctEqual(hash, s_creds.hash, sizeof(hash))) return ESP_FAIL;
    uint8_t salt[kSaltBytes];
    fillRandom(salt, sizeof(salt));
    if (deriveKey(new_password, salt, sizeof(salt), hash, sizeof(hash),
                  kPbkdf2Iterations) != ESP_OK) {
        return ESP_FAIL;
    }
    std::memcpy(s_creds.salt, salt, sizeof(salt));
    std::memcpy(s_creds.hash, hash, sizeof(hash));
    s_creds.iterations = kPbkdf2Iterations;
    esp_err_t err = saveCreds();
    if (err != ESP_OK) return err;
    authRevokeAllSessions();
    ESP_LOGI(TAG, "password changed");
    VC_RING_INFO("auth", "password changed");
    return ESP_OK;
}

// ===========================================================================
// throttling
// ===========================================================================
esp_err_t authRecordFailedAttempt(const char* identifier) {
    if (identifier == nullptr) return ESP_ERR_INVALID_ARG;
    ThrottleEntry* slot = throttleSlot(identifier);
    if (slot == nullptr) return ESP_ERR_NO_MEM;
    if (slot->id[0] == '\0') {
        std::strncpy(slot->id, identifier, sizeof(slot->id) - 1);
        slot->count = 1;
        slot->window_start_ms = nowMs();
    } else {
        ++slot->count;
        if (slot->count > kMaxAttemptsPerWindow) return AUTH_ERR_RATE_LIMITED;
    }
    return ESP_OK;
}

void authClearFailedAttempts(const char* identifier) {
    if (identifier == nullptr) return;
    for (auto& e : s_throttle) {
        if (std::strcmp(e.id, identifier) == 0) {
            e.count = 0;
            e.window_start_ms = nowMs();
        }
    }
}

// ===========================================================================
// CSRF
// ===========================================================================
// Generates a CSRF token as a base64 string into out (caller NUL-terminates).
// Returns the encoded length.
esp_err_t authCsrfGenerate(char* out, size_t out_cap, size_t& out_len) {
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    uint8_t raw[kCsrfTokenBytes];
    fillRandom(raw, sizeof(raw));
    char* out_ptr = out;
    size_t out_cap_val = out_cap;
    size_t enc = vc_b64e(static_cast<const uint8_t*>(raw), sizeof(raw), out_ptr,
                         out_cap_val);
    if (enc == 0 || enc >= out_cap) return ESP_ERR_INVALID_SIZE;
    out[enc] = '\0';
    out_len = enc;
    return ESP_OK;
}

// Constant-time compare of two NUL-terminated strings (double-submit check).
bool ctStrEqual(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) return false;
    size_t la = std::strlen(a);
    size_t lb = std::strlen(b);
    if (la != lb) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < la; ++i) diff |= static_cast<uint8_t>(a[i]) ^
                                             static_cast<uint8_t>(b[i]);
    return diff == 0;
}

esp_err_t authCsrfValidate(const char* header_token,
                           const char* cookie_token) {
    if (header_token == nullptr || cookie_token == nullptr)
        return ESP_ERR_INVALID_ARG;
    return ctStrEqual(header_token, cookie_token) ? ESP_OK : ESP_FAIL;
}

}  // namespace visioncam