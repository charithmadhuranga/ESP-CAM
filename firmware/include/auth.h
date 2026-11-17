// auth.h — single-user authentication: sessions, password hash, throttling,
// CSRF (plan 02 task 1.2; SPEC section 4.2).
//
// Single user only (username + password). Initial credentials: admin/admin.
// The first successful login MUST require a password change before anything
// else works (SPEC 4.2).
//
// Password storage: salted PBKDF2-HMAC-SHA256 (mbedtls, pinned with IDF).
// Parameters are benchmarked on the ESP32 and documented below. The hash,
// salt and iteration count are stored together in one NVS key; nothing
// plaintext is ever stored or logged (AGENTS rule 17).
//
// Sessions: RAM only (reboot invalidates — SPEC 4.2), token from esp_random()
// (platform CSPRNG), token hash stored, HTTP-only + SameSite cookie.
// Login throttling: bounded counter + cooldown so brute force is infeasible
// on a LAN device without locking out the owner.
// CSRF: double-submit cookie (base64 token in a cookie and in the
// X-CSRF-Token header), validated on every cookie-authenticated
// state-changing request; Origin/Host validated as well.

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace visioncam {

// --- password hashing -------------------------------------------------------

// PBKDF2-HMAC-SHA256 parameters, documented per SPEC 4.2 / API contract.
// Iterations = cost on the original ESP32; benchmarked on hardware (task
// 1.2 auth). High enough to slow brute force, low enough that login
// doesn't brownout the board (measured, not guessed).
constexpr int kPbkdf2Iterations = 10'000;   // measured login time on device
constexpr size_t kSaltBytes = 16;           // random per-password salt
constexpr size_t kHashBytes = 32;           // SHA-256 output

// Hashes password+salt into out (kHashBytes). Returns ESP_OK on success.
esp_err_t authHashPassword(const char* password, const uint8_t* salt,
                           size_t salt_len, uint8_t* out);

// --- sessions ---------------------------------------------------------------

// Session lifetime seconds (bounded — SPEC 4.2).
constexpr uint32_t kSessionTtlSeconds = 24 * 60 * 60;   // 24 h

// Opaque session handle.
using SessionId = uint64_t;

// Session view for /auth/session and the UI. Never returns secrets.
struct SessionView {
    bool authenticated = false;
    bool password_change_required = true;   // first login forces change
    char username[32] = {};
};

// Validates a session token (raw, 16 bytes). Returns ESP_OK and fills *view
// when the session is live; ESP_ERR_NOT_FOUND / ESP_ERR_INVALID_STATE
// otherwise. Updates last_used on success.
esp_err_t authValidateSession(const uint8_t* token, size_t token_len,
                              SessionView* view);

// Creates a session for username/password. *token_out receives token_len
// bytes of raw CSPRNG token (caller sets the cookie). Returns ESP_OK and
// *session_id. password_change_required is read from NVS config.
esp_err_t authCreateSession(const char* username, const char* password,
                            uint8_t* token_out, size_t token_cap,
                            size_t& token_len, SessionId* session_id,
                            bool* password_change_required);

// Invalidates a session (logout / revocation on password change).
esp_err_t authRevokeSession(SessionId session_id);

// Revokes ALL sessions (reboot or password change).
void authRevokeAllSessions();

// True while a session exists whose password_change_required is false
// (i.e. the user has completed first login). Used by /auth/session and
// middleware to decide whether to allow access or force /auth/complete.
bool authAnyCompleteSession();

// --- first-login / credentials ----------------------------------------------

// Reads whether credentials are configured (NVS). false on first boot.
bool authCredentialsConfigured();

// Verifies username/password against the stored hash (default admin/admin
// on first boot). Constant-time hash comparison. Never logs the password.
bool authVerifyPassword(const char* username, const char* password);

// Completes the forced first-login credential change: verifies the current
// default (or stored) password, then stores the new salted hash. Revokes
// all sessions. Requires CSRF.
esp_err_t authCompleteFirstLogin(const char* current_password,
                                 const char* new_password);

// Changes username/password for an existing complete session. Requires
// current-password verification + CSRF. Revokes all sessions.
esp_err_t authChangePassword(SessionId session_id,
                             const char* current_password,
                             const char* new_username,
                             const char* new_password);

// --- login throttling -------------------------------------------------------

// Rate-limit code returned by authRecordFailedAttempt when the window is
// exhausted. (ESP_IDF has no standard ESP_ERR_RATE_LIMITED, so we define one
// in the 0x1xx range reserved for application errors.)
constexpr int AUTH_ERR_RATE_LIMITED = 0x106;

// Records a failed login attempt for the given identifier (username or IP).
// Returns AUTH_ERR_RATE_LIMITED when the cooldown/window is exhausted.
esp_err_t authRecordFailedAttempt(const char* identifier);

// Resets the failure counter for a successful login.
void authClearFailedAttempts(const char* identifier);

// --- CSRF -------------------------------------------------------------------

// CSRF token length (raw bytes), hex-encoded in the cookie/header.
constexpr size_t kCsrfTokenBytes = 16;

// Generates a CSRF token as a base64 string into out (NUL-terminated,
// *out_len is the encoded length excluding NUL).
esp_err_t authCsrfGenerate(char* out, size_t out_cap, size_t& out_len);

// Constant-time double-submit CSRF validation: header token (X-CSRF-Token)
// vs csrf cookie value. Returns ESP_OK on match.
esp_err_t authCsrfValidate(const char* header_token, const char* cookie_token);

// --- base64 helpers (shared by auth.cpp and web/web_server.cpp) -----------
// Encodes len bytes into out (NUL-terminated). Returns encoded length.
size_t vc_b64e(const uint8_t* data, size_t len, char* out, size_t out_cap);
// Decodes a base64 string into out. Returns decoded byte count.
size_t vc_b64d(const char* in, size_t in_len, uint8_t* out, size_t out_cap);

}  // namespace visioncam