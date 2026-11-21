// network_manager.cpp — see network_manager.h.
//
// STRUCTURE (why the state is spread across an event handler, a
// supervisor task and atomics):
//   - esp_wifi/IP events arrive on the DEFAULT EVENT LOOP task. The
//     handler only records state and publishes a bounded event — it never
//     blocks, never connects directly (except the first start), so the
//     loop stays responsive.
//   - Reconnect attempts are owned by ONE low-priority task
//     (VC_PRIO_NETWORK) which fires esp_wifi_connect() when the backoff
//     deadline passes. A single connect caller means no attempted-vs-
//     published drift and no duplicated callbacks.
//   - Shared scalars are std::atomic (relaxed where ordering does not
//     matter — every value is a snapshot for display/logging, never a
//     protocol state machine input).
//   - No dynamic allocation anywhere: the SNTP service, netifs and the
//     driver task are created once at init and live for the boot.
//
// BACKOFF (plan 02 task 1.1.2 "exponential with jitter"): 1 s doubling
// to a 32 s cap, plus 0..500 ms of jitter from the hardware RNG so a
// whole rack of devices cannot synchronise their retries. The counter
// resets on every GOT_IP, so a stable connection costs nothing. Every
// published WifiDisconnected carries the driver's reason code as the
// event value — diagnostics without an extra log path.

#include "network_manager.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"

#include "event_bus.h"
#include "events.h"
#include "ring_log.h"
#include "tasks.h"
#include "wifi_credentials.h"

namespace visioncam {
namespace {

const char* TAG = "vc_net";

// --- bounded, atomic shared state ----------------------------------------
std::atomic<WifiState>   s_state{WifiState::Off};
std::atomic<bool>        s_radio_up{false};
std::atomic<bool>        s_configured{false};
std::atomic<bool>        s_ap_mode{false};       // true when running SoftAP
std::atomic<bool>        s_time_synced{false};
std::atomic<bool>        s_sntp_started{false};
std::atomic<bool>        s_init_done{false};
std::atomic<bool>        s_provisioning{false};
std::atomic<bool>        s_provision_pending{false};
std::atomic<bool>        s_network_task_started{false};

std::atomic<uint32_t>    s_connects{0};
std::atomic<uint32_t>    s_disconnects{0};
std::atomic<uint32_t>    s_attempts{0};        // esp_wifi_connect() calls
std::atomic<uint32_t>    s_reconnect_errors{0};
std::atomic<int>         s_last_connect_err{ESP_OK};
std::atomic<uint32_t>    s_backoff_ms{0};
std::atomic<int64_t>     s_reconnect_at{0};    // ms since boot; 0 = none
std::atomic<int64_t>     s_last_change{0};

// Monotonic-ish millisecond clock (wraps at ~49 days like the event bus
// timestamps; ordering needs no better resolution).
int64_t nowMs() { return esp_timer_get_time() / 1000; }

void setState(WifiState st) {
    s_state.store(st);
    s_last_change.store(nowMs());
}

// Event + ring-log pairs are written directly at each site with
// eventBusPublish() + VC_RING_*: both are non-blocking, the bus drops the
// oldest event when full (event_bus.h) and the ring drops the oldest line.

// --- SNTP -----------------------------------------------------------------
// esp_netif's SNTP service (IDF 6.1: the standalone esp_sntp component
// is gone — its API now lives in esp_netif/lwip, SPEC section 1A's
// dependency name is satisfied by this include). Started on the first
// GOT_IP, never before: with no route there is no NTP server to reach.
constexpr char kNtpServer[] = "pool.ntp.org";

void onTimeSync(struct timeval* tv) {
    s_time_synced.store(true);
    // Quick, non-blocking: a flag and one line. The callback runs in the
    // SNTP task context; nothing here waits on anything.
    ESP_LOGI(TAG, "NTP time sync OK (%lld.%06ld s)", (long long)tv->tv_sec,
             (long)tv->tv_usec);
    VC_RING_INFO("ntp", "time sync ok");
}

esp_err_t sntpStartOnce() {
    bool expected = false;
    if (!s_sntp_started.compare_exchange_strong(expected, true)) return ESP_OK;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(kNtpServer);
    cfg.start = false;          // controlled start below
    cfg.wait_for_sync = false;  // no semaphore needed; the callback is enough
    cfg.sync_cb = onTimeSync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) return err;
    return esp_netif_sntp_start();
}

// --- AP mode (fallback when no credentials are configured) ------------------
// Starts a SoftAP so the device is directly reachable for initial setup.
// SSID is "VisionCam-XXXXXX" from the last 3 bytes of the station MAC so
// multiple devices on the same bench don't collide. WPA2-PSK with the
// documented setup passphrase; do not print the passphrase to logs.
esp_err_t apStart() {
    esp_netif_t* ap = esp_netif_create_default_wifi_ap();
    if (ap == nullptr) return ESP_FAIL;

    // Set AP IP to 192.168.4.1 (classic SoftAP address).
    esp_netif_ip_info_t ip_info{};
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_set_ip_info(ap, &ip_info);

    // DHCP server for AP clients.
    esp_netif_dhcps_start(ap);

    // Build SSID from MAC last 3 bytes.
    uint8_t mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char ssid[33] = {};
    std::snprintf(ssid, sizeof(ssid), "VisionCam-%02X%02X%02X",
                  mac[3], mac[4], mac[5]);

    wifi_config_t ap_cfg = {};
    std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.ssid), ssid,
                 sizeof(ap_cfg.ap.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.password),
                 "visioncam", sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.channel = 1;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) return err;

    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    s_radio_up.store(true);
    s_ap_mode.store(true);
    setState(WifiState::Ap);

    ESP_LOGI(TAG, "AP mode: ssid=%s ip=192.168.4.1", ssid);
    VC_RING_INFO("wifi", "AP mode ssid=%s ip=192.168.4.1", ssid);
    return ESP_OK;
}


void onStarted() {
    if (!s_configured.load() && !s_provisioning.load()) {
        setState(WifiState::Ap);
        return;
    }
    setState(WifiState::Connecting);
    s_reconnect_at.store(0);
    // First attempt immediately: the event loop calls connect for us, so
    // the supervisor task stays idle until a real disconnect arms it.
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_reconnect_errors.fetch_add(1);
        s_last_connect_err.store(err);
        s_reconnect_at.store(nowMs() + 1000);  // supervisor retries in 1 s
        ESP_LOGW(TAG, "connect failed at start: %s", esp_err_to_name(err));
    } else {
        s_attempts.fetch_add(1);
        s_backoff_ms.store(0);
    }
}

void onGotIp(const ip_event_got_ip_t* got) {
    char ip[16] = {0};
    esp_ip4addr_ntoa(&got->ip_info.ip, ip, sizeof(ip));
    int rssi_int = 0;
    esp_wifi_sta_get_rssi(&rssi_int);
    const int8_t rssi = static_cast<int8_t>(rssi_int);
    s_connects.fetch_add(1);
    s_backoff_ms.store(0);
    s_reconnect_at.store(0);
    setState(WifiState::Connected);

    eventBusPublish(EventType::WifiConnected,
                    static_cast<uint16_t>(static_cast<int16_t>(rssi)));
    VC_RING_INFO("wifi", "connected rssi=%d", static_cast<int>(rssi));
    ESP_LOGI(TAG, "GOT_IP %s (rssi %d dBm, connects=%u)", ip,
             static_cast<int>(rssi),
             static_cast<unsigned>(s_connects.load()));

    const esp_err_t serr = sntpStartOnce();
    if (serr != ESP_OK) {
        // Not fatal: timestamps stay monotonic until a retry lands.
        ESP_LOGW(TAG, "sntp start failed: %s", esp_err_to_name(serr));
    }
}

void onDisconnected(uint8_t reason) {
    s_disconnects.fetch_add(1);
    if (s_provisioning.load()) {
        s_reconnect_at.store(0);
        setState(WifiState::Ap);
    } else if (s_ap_mode.load() && !s_configured.load()) {
        setState(WifiState::Ap);
    } else {
        setState(WifiState::Disconnected);
    }

    eventBusPublish(EventType::WifiDisconnected, reason);
    VC_RING_WARN("wifi", "disconnected reason=%u",
                 static_cast<unsigned>(reason));
    ESP_LOGW(TAG, "disconnected (reason %u, disconnects=%u)",
             static_cast<unsigned>(reason),
             static_cast<unsigned>(s_disconnects.load()));

    if (s_provisioning.load() ||
        (s_ap_mode.load() && !s_configured.load())) {
        return;
    }

    // Exponential backoff + jitter, rescheduled by the supervisor task
    // after each failed attempt (attempt counter resets on GOT_IP).
    uint32_t n = s_attempts.load();
    if (n > 5) n = 5;
    const uint32_t delay = 1000u << n;
    const uint32_t jitter = (delay >= 32000u) ? 0u : (esp_random() % 500u);
    s_backoff_ms.store(delay);
    s_reconnect_at.store(nowMs() + static_cast<int64_t>(delay + jitter));
    ESP_LOGI(TAG, "reconnect scheduled in %u ms (+jitter)", delay);
}

void onWifiEvent(void* /*arg*/, esp_event_base_t base, int32_t id,
                 void* event_data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            onStarted();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const auto* d = static_cast<const wifi_event_sta_disconnected_t*>(
                event_data);
            onDisconnected(d->reason);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT) {
        switch (id) {
        case IP_EVENT_STA_GOT_IP:
            onGotIp(static_cast<const ip_event_got_ip_t*>(event_data));
            break;
        case IP_EVENT_STA_LOST_IP:
            if (s_state.load() == WifiState::Connected) {
                setState(WifiState::Disconnected);
                s_reconnect_at.store(nowMs() + 1000);
                s_backoff_ms.store(1000);
                VC_RING_WARN("wifi", "ip lost — reconnecting");
            }
            break;
        default:
            break;
        }
    }
}

// --- supervisor task: the only place esp_wifi_connect() is called -------
void networkTask(void* /*arg*/) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (!s_radio_up.load()) continue;
        if (s_state.load() == WifiState::Connected) continue;

        const int64_t due = s_reconnect_at.load();
        if (due == 0) continue;
        if (nowMs() < due) continue;

        // Claim the slot before calling: the handler can re-arm it while
        // we are in esp_wifi_connect(), and that must not double-fire.
        s_reconnect_at.store(0);
        const esp_err_t err = esp_wifi_connect();
        if (err == ESP_OK) {
            s_attempts.fetch_add(1);
            setState(WifiState::Connecting);
        } else {
            s_reconnect_errors.fetch_add(1);
            s_last_connect_err.store(err);
            s_reconnect_at.store(nowMs() + 500);  // retry the retry soon
            ESP_LOGW(TAG, "reconnect failed: %s", esp_err_to_name(err));
        }
    }
}

void startNetworkTask() {
    bool expected = false;
    if (!s_network_task_started.compare_exchange_strong(expected, true)) return;
    const BaseType_t ok = xTaskCreate(networkTask, "vc_netrecon",
                                      VC_STACK_NETWORK, nullptr,
                                      VC_PRIO_NETWORK, nullptr);
    if (ok != pdPASS) {
        s_network_task_started.store(false);
        ESP_LOGE(TAG, "network task create failed — reconnects disabled");
        VC_RING_ERROR("wifi", "netrecon task create failed — reconnects off");
    }
}

}  // namespace

const char* wifiStateName(WifiState state) {
    switch (state) {
    case WifiState::Off:           return "Off";
    case WifiState::Connecting:    return "Connecting";
    case WifiState::Connected:     return "Connected";
    case WifiState::Disconnected:  return "Disconnected";
    case WifiState::Ap:            return "AP";
    }
    return "Unknown";
}

esp_err_t networkManagerInit() {
    if (s_init_done.exchange(true)) return ESP_ERR_INVALID_STATE;

    char ssid[33] = {0};
    char pass[64] = {0};
    size_t ssid_len = 0, pass_len = 0;
    bool found = false;
    bool staged_applied = false;
    esp_err_t err = wifiCredentialsApplyStaged(staged_applied);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pending Wi-Fi setup could not be committed: %s",
                 esp_err_to_name(err));
    } else if (staged_applied) {
        ESP_LOGI(TAG, "verified Wi-Fi setup applied before camera initialization");
    }
    err = wifiCredentialsLoad(ssid, sizeof(ssid), pass, sizeof(pass),
                              ssid_len, pass_len, found);
    if (err != ESP_OK) {
        // A real NVS fault — the caller decides how degraded to run.
        ESP_LOGE(TAG, "credential load failed: %s", esp_err_to_name(err));
        return err;
    }
    s_configured.store(found);

    // lwIP (esp_netif) is ALWAYS initialized: the web server needs it even
    // in local-only mode (no credentials). Initializing it here, before
    // the web server starts, prevents the tcpip "Invalid mbox" assert.
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    if (!found) {
        // No credentials: fall back to SoftAP mode so the device is directly
        // reachable for initial setup (SPEC 9 degraded operation — the
        // camera, recording and UI all work locally regardless).
        esp_netif_init();
        esp_event_loop_create_default();

        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&init_cfg);
        if (err != ESP_OK) return err;

        esp_netif_t* sta = esp_netif_create_default_wifi_sta();
        if (sta == nullptr) return ESP_FAIL;
        esp_netif_set_hostname(sta, "visioncam");

        esp_event_handler_instance_t unused;
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                  &onWifiEvent, nullptr, &unused);
        if (err != ESP_OK) return err;
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                  &onWifiEvent, nullptr, &unused);
        if (err != ESP_OK) return err;
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                                  &onWifiEvent, nullptr, &unused);
        if (err != ESP_OK) return err;

        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) return err;
        s_ap_mode.store(true);
        err = apStart();
        if (err != ESP_OK) return err;

        ESP_LOGI(TAG, "no credentials — setup AP active at 192.168.4.1");
        VC_RING_INFO("wifi", "no credentials — setup AP active");
        return ESP_OK;
    }
    // Lengths only — NEVER the values (AGENTS rule 17).
    ESP_LOGI(TAG, "credentials loaded (ssid %u B, pass %u B) — starting radio",
             static_cast<unsigned>(ssid_len), static_cast<unsigned>(pass_len));

    esp_netif_t* sta = esp_netif_create_default_wifi_sta();
    if (sta == nullptr) return ESP_FAIL;
    esp_netif_set_hostname(sta, "visioncam");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) return err;

    esp_event_handler_instance_t unused;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &onWifiEvent, nullptr, &unused);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &onWifiEvent, nullptr, &unused);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                              &onWifiEvent, nullptr, &unused);
    if (err != ESP_OK) return err;

    wifi_config_t sta_cfg = {};
    std::strncpy(reinterpret_cast<char*>(sta_cfg.sta.ssid), ssid,
                 sizeof(sta_cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(sta_cfg.sta.password), pass,
                 sizeof(sta_cfg.sta.password) - 1);
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) return err;

    // THE HAZARD-SENSITIVE CALL: this runs at init, i.e. before the camera
    // ISR exists (network_manager.h). Never move it below camera bring-up
    // and never call it again while the camera streams.
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_radio_up.store(true);
    setState(WifiState::Connecting);

    // Modem sleep: less brownout headroom needed under idle association.
    // Failure is non-fatal (the radio still works, just less politely).
    const esp_err_t perr = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (perr != ESP_OK) ESP_LOGW(TAG, "set_ps failed: %s", esp_err_to_name(perr));

    // Single reconnect owner (see the task comment above).
    startNetworkTask();

    VC_RING_INFO("wifi", "radio starting (ssid %u B)",
                 static_cast<unsigned>(ssid_len));
    ESP_LOGI(TAG, "radio starting in STA mode (first connect from event handler)");
    return ESP_OK;
}

esp_err_t networkManagerProvision(const char* ssid, const char* pass) {
    if (ssid == nullptr || pass == nullptr) return ESP_ERR_INVALID_ARG;
    const size_t ssid_len = std::strlen(ssid);
    const size_t pass_len = std::strlen(pass);
    if (ssid_len == 0 || ssid_len > 32 ||
        (pass_len != 0 && (pass_len < 8 || pass_len > 63))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ap_mode.load() || s_configured.load() ||
        s_provision_pending.load() || !s_radio_up.load()) {
        return ESP_ERR_INVALID_STATE;
    }
    bool expected = false;
    if (!s_provisioning.compare_exchange_strong(expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t sta_cfg = {};
    std::memcpy(sta_cfg.sta.ssid, ssid, ssid_len);
    std::memcpy(sta_cfg.sta.password, pass, pass_len);
    s_reconnect_at.store(0);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    std::memset(&sta_cfg, 0, sizeof(sta_cfg));
    if (err != ESP_OK) {
        s_provisioning.store(false);
        setState(WifiState::Ap);
        return err;
    }

    setState(WifiState::Connecting);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_provisioning.store(false);
        setState(WifiState::Ap);
        return err;
    }

    constexpr uint32_t kConnectTimeoutMs = 15000;
    constexpr uint32_t kPollIntervalMs = 100;
    for (uint32_t elapsed = 0; elapsed < kConnectTimeoutMs;
         elapsed += kPollIntervalMs) {
        const WifiState state = s_state.load();
        if (state == WifiState::Connected) break;
        if (state == WifiState::Ap) break;
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }

    if (s_state.load() != WifiState::Connected) {
        s_reconnect_at.store(0);
        wifi_config_t empty_cfg = {};
        const esp_err_t clear_err = esp_wifi_set_config(WIFI_IF_STA, &empty_cfg);
        s_provisioning.store(false);
        setState(WifiState::Ap);
        return clear_err == ESP_OK ? ESP_ERR_TIMEOUT : clear_err;
    }

    err = wifiCredentialsStageForBoot(ssid, pass);
    if (err != ESP_OK) {
        s_reconnect_at.store(0);
        esp_wifi_disconnect();
        wifi_config_t empty_cfg = {};
        const esp_err_t clear_err = esp_wifi_set_config(WIFI_IF_STA, &empty_cfg);
        s_provisioning.store(false);
        setState(WifiState::Ap);
        return clear_err == ESP_OK ? err : clear_err;
    }

    s_provision_pending.store(true);
    s_provisioning.store(false);
    s_ap_mode.store(true);
    setState(WifiState::Connected);
    return ESP_OK;
}

esp_err_t networkManagerStopSetupAp() {
    if (!s_provision_pending.load() ||
        s_state.load() != WifiState::Connected ||
        !s_ap_mode.load() || !s_radio_up.load()) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not stop setup AP: %s", esp_err_to_name(err));
        return err;
    }

    s_ap_mode.store(false);
    ESP_LOGI(TAG, "setup AP stopped after station obtained an IP");
    VC_RING_INFO("wifi", "setup AP stopped after station obtained an IP");
    return ESP_OK;
}

esp_err_t networkManagerStop() {
    if (!s_radio_up.load()) return ESP_OK;  // already off counts as success
    // Hazard note (network_manager.h): the caller MUST have shut the
    // camera down (or accepted the documented stop-with-camera-up risk).
    const esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) return err;
    s_radio_up.store(false);
    s_reconnect_at.store(0);
    setState(WifiState::Off);
    eventBusPublish(EventType::WifiDisconnected, 0, err);
    VC_RING_INFO("wifi", "radio stopped");
    ESP_LOGI(TAG, "radio stopped");
    return ESP_OK;
}

WifiStatus networkManagerStatus() {
    WifiStatus st;
    st.state = s_state.load();
    st.configured = s_configured.load();
    st.radio_up = s_radio_up.load();
    st.ap_mode = s_ap_mode.load();
    st.provision_pending = s_provision_pending.load();
    st.time_synced = s_time_synced.load();
    st.connects = s_connects.load();
    st.disconnects = s_disconnects.load();
    st.attempts = s_attempts.load();
    st.reconnect_errors = s_reconnect_errors.load();
    st.backoff_ms = s_backoff_ms.load();
    st.last_change_ms = static_cast<uint32_t>(s_last_change.load());
    st.last_connect_err = static_cast<esp_err_t>(s_last_connect_err.load());
    if (st.radio_up && st.state == WifiState::Connected) {
        int rssi_int = 0;
        if (esp_wifi_sta_get_rssi(&rssi_int) == ESP_OK) {
            st.rssi = static_cast<int8_t>(rssi_int);
        }
    }
    return st;
}

bool networkManagerRadioOwned() { return s_radio_up.load(); }

}  // namespace visioncam