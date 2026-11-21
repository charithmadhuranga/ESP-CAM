// network_manager.h — Wi-Fi manager: station mode, NTP, reconnect with
// exponential backoff + jitter (plan 02 task 1.1.2; SPEC 4.1 local-first,
// 8.3 event bus, DESIGN "real connection" status rule).
//
// BOOT-ORDER REQUIREMENT (docs/REFERENCE_ANALYSIS.md, "Camera driver ISR
// overflow warning vs flash-cache-off windows"): the radio is started by
// networkManagerInit(), which must run BEFORE the camera is initialised.
// esp_wifi_start() initialises the radio and reads NVS with the flash
// cache disabled; that window must never overlap the camera DMA ISR —
// reproduced as three byte-identical panics when Wi-Fi was started while
// the camera streamed (docs/evidence/phase1-core-boot-03.log). After init
// the radio stays up: reconnects only call esp_wifi_connect() (pure
// association, no NVS), which cannot re-enter that window. Stopping the
// radio is the shutdown path — call networkManagerStop() only with the
// camera down, or serialise it with the camera (task 1.2's config
// endpoints must do exactly that; the RTC ring keeps the evidence either
// way).
//
// STATE SEMANTICS (DESIGN.md: a status is "Connected" only when a real
// connection succeeded, never because a setting was saved): Connected is
// entered ONLY on IP_EVENT_STA_GOT_IP. Association without an IP address
// stays Connecting. Nothing is inferred from configuration alone.
//
// No credentials live here: they come from NVS via wifi_credentials.h and
// are never logged (AGENTS rule 17).
//
// Degraded operation (SPEC 9): with no configured SSID the manager starts
// a setup SoftAP; the authenticated web UI can provision a network. Losing
// the network never stops a camera task: events are published to the
// bounded bus and reconnection happens on this module's own low-priority task.

#pragma once

#include <cstdint>

#include "esp_err.h"

namespace visioncam {

enum class WifiState : uint8_t {
    Off = 0,         // radio not started: explicitly stopped
    Connecting,      // radio up, associating (or associated without an IP)
    Connected,       // GOT_IP: a real address (DESIGN "real connection" rule)
    Disconnected,    // was up, lost it; retrying on backoff
    Ap,              // SoftAP mode: no credentials configured, device is its
                     // own access point for direct configuration
};

// Human-readable state name for logs, API responses and the UI payload.
// Never returns nullptr.
const char* wifiStateName(WifiState state);

// Point-in-time view for /status, /health and the UI. rssi is refreshed
// from the driver when the radio is up (dBm, negative; 0 when unknown).
struct WifiStatus {
    WifiState  state = WifiState::Off;
    bool       configured = false;    // a usable SSID exists in NVS
    bool       radio_up = false;      // esp_wifi driver started by this module
    bool       ap_mode = false;       // true when setup SoftAP is running
    bool       provision_pending = false;  // verified; awaiting software reboot
    bool       time_synced = false;   // SNTP reported a successful sync
    int8_t     rssi = 0;              // dBm, valid while Connected
    uint32_t   connects = 0;          // GOT_IP count since boot
    uint32_t   disconnects = 0;       // disconnect events since boot
    uint32_t   attempts = 0;          // esp_wifi_connect() calls since boot
    uint32_t   reconnect_errors = 0;  // esp_wifi_connect() failures
    uint32_t   last_change_ms = 0;    // esp_timer ms of the last state change
    uint32_t   backoff_ms = 0;        // reconnect delay in force (0 when up)
    esp_err_t  last_connect_err = ESP_OK;  // driver error from the last start()
};

// Loads credentials and initialises Wi-Fi in station mode, or starts the
// setup SoftAP in AP+station mode when none exist. Call ONCE, early: after
// coreInit() (needs the event bus) and BEFORE camera bring-up (hazard
// note above). When no SSID is configured WifiState::Ap is returned without
// error — "not configured" is a valid setup state, not a failure.
// Idempotent: a second call returns ESP_ERR_INVALID_STATE.
esp_err_t networkManagerInit();

// While in setup AP mode, associates with the supplied network and persists
// stages it for the next software reboot only after receiving an IP address.
// NVS is updated on the next boot before camera initialization, avoiding a
// flash-cache-off write while the camera ISR is active.
esp_err_t networkManagerProvision(const char* ssid, const char* pass);

// Switch from AP+STA to STA after a successful provisioning response.
// Requires a staged network that has a valid station IP.
esp_err_t networkManagerStopSetupAp();

// Disconnects and stops the radio. NEVER call this while the camera is
// streaming (hazard note above). Succeeds when already off.
esp_err_t networkManagerStop();

// Snapshot of the live state. Safe to call from any task; refreshes RSSI
// from the driver when it can.
WifiStatus networkManagerStatus();

// True when this module started the Wi-Fi driver (radio owned here).
// The Phase 0 smoke test uses it to decide whether it may drive the
// radio itself (SoftAP) for its TX-load probe.
bool networkManagerRadioOwned();

}  // namespace visioncam