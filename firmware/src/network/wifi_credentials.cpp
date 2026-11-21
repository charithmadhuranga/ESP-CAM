// wifi_credentials.cpp — see wifi_credentials.h.

#include "wifi_credentials.h"

#include <cstring>

#include "esp_attr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

namespace visioncam {
namespace {

const char* TAG = "vc_wificred";

// Ours (SPEC 7: single authoritative NVS store).
constexpr char kNsVisioncam[]  = "visioncam";
constexpr char kKeySsid[]      = "wifi_ssid";
constexpr char kKeyPass[]      = "wifi_pass";

// Phase 0 provisioning (tools/nvs_provision, R1's namespace) — one-time
// migration source only; see the header for why this exists and when it
// is removed.
constexpr char kNsLegacy[]     = "ESP-CAM_MJPEG";
constexpr char kKeyLegacySsid[] = "ST_SSID";
constexpr char kKeyLegacyPass[] = "ST_Pass";

constexpr size_t kSsidMax = 32;   // ESP32 Wi-Fi driver maximum
constexpr size_t kPassMax = 63;   // WPA passphrase maximum (64 with NUL)
constexpr uint32_t kPendingMagic = 0x56435746;  // "VCWF"

struct PendingCredentials {
    uint32_t magic;
    uint8_t ssid_len;
    uint8_t pass_len;
    char ssid[33];
    char pass[64];
    uint32_t checksum;
};

RTC_NOINIT_ATTR PendingCredentials s_pending;

uint32_t pendingChecksum(const PendingCredentials& pending) {
    uint32_t hash = 2166136261u;
    const auto mix = [&hash](uint8_t value) {
        hash = (hash ^ value) * 16777619u;
    };
    mix(pending.ssid_len);
    mix(pending.pass_len);
    for (size_t i = 0; i < pending.ssid_len; ++i) {
        mix(static_cast<uint8_t>(pending.ssid[i]));
    }
    for (size_t i = 0; i < pending.pass_len; ++i) {
        mix(static_cast<uint8_t>(pending.pass[i]));
    }
    return hash;
}

bool validCredentials(const char* ssid, const char* pass) {
    if (ssid == nullptr || pass == nullptr) return false;
    const size_t ssid_len = std::strlen(ssid);
    const size_t pass_len = std::strlen(pass);
    return ssid_len != 0 && ssid_len <= kSsidMax &&
           (pass_len == 0 || (pass_len >= 8 && pass_len <= kPassMax));
}

// Reads a string key into a fixed buffer. Never allocates.
// Returns ESP_OK and writes the value, or ESP_ERR_NVS_NOT_FOUND. NOTE the
// NVS-specific code: the generic ESP_ERR_NOT_FOUND is a DIFFERENT number,
// and comparing against it silently turns "key missing" into a fatal error.
// A value that does not fit is an error (never silently truncated).
esp_err_t readStr(nvs_handle_t h, const char* key, char* buf, size_t cap,
                  size_t& len_out) {
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, buf, &len);
    if (err != ESP_OK) return err;
    if (len == 0 || len > cap) return ESP_ERR_INVALID_SIZE;
    len_out = len;
    return ESP_OK;
}

// Best-effort copy of a migrated credential into our own namespace.
// Failure is logged and ignored: the caller still uses the value in RAM.
void migrateToVisioncam(const char* ssid, const char* pass) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNsVisioncam, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "migrate: open failed (%s) — using in RAM only",
                 esp_err_to_name(err));
        return;
    }
    if (nvs_set_str(h, kKeySsid, ssid) == ESP_OK &&
        nvs_set_str(h, kKeyPass, pass) == ESP_OK) {
        err = nvs_commit(h);
    } else {
        err = ESP_FAIL;
    }
    nvs_close(h);
    if (err == ESP_OK) {
        // Lengths only — never the values (AGENTS rule 17).
        ESP_LOGI(TAG, "migrated legacy credentials into our namespace "
                      "(ssid %u B, pass %u B)",
                 static_cast<unsigned>(std::strlen(ssid)),
                 static_cast<unsigned>(std::strlen(pass)));
    } else {
        ESP_LOGW(TAG, "migrate: commit failed (%s) — using in RAM only",
                 esp_err_to_name(err));
    }
}

}  // namespace

esp_err_t wifiCredentialsLoad(char* ssid, size_t ssid_cap,
                              char* pass, size_t pass_cap,
                              size_t& ssid_len, size_t& pass_len,
                              bool& found) {
    ssid_len = 0;
    pass_len = 0;
    found = false;
    if (ssid_cap == 0 || pass_cap == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(kNsVisioncam, NVS_READONLY, &h);
    if (err != ESP_OK) {
        // A namespace that does not exist yet is "nothing stored", which
        // is the first-boot state; anything else is a real NVS fault.
        if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    } else {
        esp_err_t serr = readStr(h, kKeySsid, ssid, ssid_cap, ssid_len);
        esp_err_t perr = readStr(h, kKeyPass, pass, pass_cap, pass_len);
        nvs_close(h);
        if (serr == ESP_OK && perr == ESP_OK) {
            if (ssid_len > kSsidMax || pass_len > kPassMax) return ESP_ERR_INVALID_SIZE;
            found = true;
            return ESP_OK;
        }
        if (serr != ESP_OK && serr != ESP_ERR_NVS_NOT_FOUND) return serr;
        if (perr != ESP_OK && perr != ESP_ERR_NVS_NOT_FOUND) return perr;
        // Half-stored config (SSID without passphrase) is not usable:
        // fall through to the migration fallback rather than trust it.
    }

    // --- one-time migration fallback: Phase 0 provisioning --------------
    err = nvs_open(kNsLegacy, NVS_READONLY, &h);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "no credentials configured — local-only operation");
            return ESP_OK;
        }
        return err;
    }
    esp_err_t serr = readStr(h, kKeyLegacySsid, ssid, ssid_cap, ssid_len);
    esp_err_t perr = readStr(h, kKeyLegacyPass, pass, pass_cap, pass_len);
    nvs_close(h);
    if (serr != ESP_OK) {
        if (serr == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;  // nothing there
        return serr;
    }
    if (perr != ESP_OK && perr != ESP_ERR_NVS_NOT_FOUND) return perr;
    if (ssid_len > kSsidMax || pass_len > kPassMax) return ESP_ERR_INVALID_SIZE;

    found = true;
    migrateToVisioncam(ssid, pass);
    return ESP_OK;
}

esp_err_t wifiCredentialsSave(const char* ssid, const char* pass) {
    if (!validCredentials(ssid, pass)) return ESP_ERR_INVALID_ARG;
    const size_t ssid_len = std::strlen(ssid);
    const size_t pass_len = std::strlen(pass);

    nvs_handle_t h;
    esp_err_t err = nvs_open(kNsVisioncam, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, kKeySsid, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, kKeyPass, pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi credentials saved (ssid %u B, pass %u B)",
                 static_cast<unsigned>(ssid_len),
                 static_cast<unsigned>(pass_len));
    }
    return err;
}

esp_err_t wifiCredentialsStageForBoot(const char* ssid, const char* pass) {
    if (!validCredentials(ssid, pass)) return ESP_ERR_INVALID_ARG;
    std::memset(&s_pending, 0, sizeof(s_pending));
    s_pending.ssid_len = static_cast<uint8_t>(std::strlen(ssid));
    s_pending.pass_len = static_cast<uint8_t>(std::strlen(pass));
    std::memcpy(s_pending.ssid, ssid, s_pending.ssid_len);
    std::memcpy(s_pending.pass, pass, s_pending.pass_len);
    s_pending.checksum = pendingChecksum(s_pending);
    __sync_synchronize();
    s_pending.magic = kPendingMagic;
    return ESP_OK;
}

esp_err_t wifiCredentialsApplyStaged(bool& applied) {
    applied = false;
    if (s_pending.magic != kPendingMagic) return ESP_OK;
    const bool valid =
        s_pending.ssid_len > 0 && s_pending.ssid_len <= kSsidMax &&
        (s_pending.pass_len == 0 ||
         (s_pending.pass_len >= 8 && s_pending.pass_len <= kPassMax)) &&
        s_pending.ssid[s_pending.ssid_len] == '\0' &&
        s_pending.pass[s_pending.pass_len] == '\0' &&
        s_pending.checksum == pendingChecksum(s_pending);
    if (!valid) {
        std::memset(&s_pending, 0, sizeof(s_pending));
        return ESP_ERR_INVALID_CRC;
    }

    const esp_err_t err = wifiCredentialsSave(s_pending.ssid, s_pending.pass);
    if (err != ESP_OK) return err;
    std::memset(&s_pending, 0, sizeof(s_pending));
    applied = true;
    return ESP_OK;
}

}  // namespace visioncam