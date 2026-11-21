// device_identity.cpp — stable device identity (Phase 7).
//
// Generated once from the base MAC (which never changes on a given chip),
// stored in NVS under its own namespace so a firmware update cannot move
// it. Falls back to "vc" + MAC-derived digits if NVS is unreadable, which
// keeps topics stable even without the stored value.

#include "device_identity.h"

#include <cstdio>
#include <cstring>

#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"

namespace visioncam {

static constexpr char kNs[] = "vc_ident";
static constexpr char kKeyId[] = "id";
static char s_id[24] = {};

static void deriveFromMac(char* out, size_t cap) {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(out, cap, "vc%02x%02x%02x", mac[3], mac[4], mac[5]);
}

const char* deviceIdentityId() {
    if (s_id[0] != '\0') return s_id;

    nvs_handle_t h = 0;
    if (nvs_open(kNs, NVS_READONLY, &h) == ESP_OK) {
        struct Guard {
            nvs_handle_t hh;
            ~Guard() { nvs_close(hh); }
        } guard{h};
        char stored[24] = {};
        size_t len = sizeof(stored);
        if (nvs_get_str(h, kKeyId, stored, &len) == ESP_OK && stored[0]) {
            std::strncpy(s_id, stored, sizeof(s_id) - 1);
            return s_id;
        }
    }
    // Not stored yet: derive from the MAC and persist it.
    deriveFromMac(s_id, sizeof(s_id));
    nvs_handle_t w = 0;
    if (nvs_open(kNs, NVS_READWRITE, &w) == ESP_OK) {
        struct Guard {
            nvs_handle_t hh;
            ~Guard() { nvs_close(hh); }
        } guard{w};
        if (nvs_set_str(w, kKeyId, s_id) == ESP_OK) nvs_commit(w);
    }
    return s_id;
}

}  // namespace visioncam