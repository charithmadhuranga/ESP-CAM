// wifi_bringup.cpp — see wifi_bringup.h.

#include "wifi_bringup.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

namespace visioncam {
namespace {

const char* TAG = "vc_wifi";

// Broadcast locally for the seconds the smoke test runs; joins nothing.
constexpr char kApSsid[] = "visioncam-smoke";
constexpr uint8_t kApChannel = 6;  // middle of the 2.4 GHz band

bool s_nvs_ready = false;
bool s_driver_up = false;  // netif + event loop + wifi driver initialised
bool s_radio_on = false;

esp_err_t ensureNvs() {
    if (s_nvs_ready) return ESP_OK;
    // The Wi-Fi calibration data lives in NVS; esp_wifi_init fails without it.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Fresh partition table on first boot: erase once and retry; the
        // calibration data regenerates on the next radio init.
        ESP_LOGW(TAG, "nvs: %s — erasing partition", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) return err;
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    s_nvs_ready = true;
    return ESP_OK;
}

}  // namespace

esp_err_t wifiStart(WifiReport& report) {
    report = WifiReport{};

    esp_err_t err = ensureNvs();
    if (err != ESP_OK) return err;

    if (!s_driver_up) {
        err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

        // AP netif for the beaconing interface, STA netif only because the
        // scan API requires it — STA never connects (no credentials exist).
        if (esp_netif_get_handle_from_ifkey("WIFI_AP_DEF") == nullptr &&
            esp_netif_create_default_wifi_ap() == nullptr) {
            return ESP_FAIL;
        }
        if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == nullptr &&
            esp_netif_create_default_wifi_sta() == nullptr) {
            return ESP_FAIL;
        }

        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&init_cfg);
        if (err != ESP_OK) return err;

        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) return err;

        wifi_config_t ap{};
        std::strncpy(reinterpret_cast<char*>(ap.ap.ssid), kApSsid,
                     sizeof(ap.ap.ssid) - 1);
        ap.ap.ssid_len = static_cast<uint8_t>(std::strlen(kApSsid));
        ap.ap.channel = kApChannel;
        ap.ap.max_connection = 1;
        ap.ap.authmode = WIFI_AUTH_OPEN;
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
        if (err != ESP_OK) return err;

        s_driver_up = true;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_radio_on = true;
    report.started = true;

    // One blocking scan: probe requests on every channel are the TX bursts
    // this test exists to survive, and the result count proves RX works too.
    // (IDF 6.1 calls this esp_wifi_scan_start — the old esp_wifi_scan name
    // no longer exists.)
    err = esp_wifi_scan_start(nullptr, true);
    if (err == ESP_OK) {
        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        report.scan_results = count;
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "SoftAP '%s' ch%u up — scan heard %u network(s)",
             kApSsid, static_cast<unsigned>(kApChannel),
             static_cast<unsigned>(report.scan_results));
    return ESP_OK;
}

esp_err_t wifiStop(WifiReport& report) {
    report = WifiReport{};
    if (!s_radio_on) return ESP_OK;  // already off counts as success
    const esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) return err;
    s_radio_on = false;
    ESP_LOGI(TAG, "radio off");
    return ESP_OK;
}

}  // namespace visioncam
