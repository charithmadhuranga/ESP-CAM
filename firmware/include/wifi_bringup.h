// wifi_bringup.h — Phase 0 smoke: prove camera + SD stay stable with Wi-Fi
// off, then on (.opencode/plans/01-phase-0-baseline.md task 0.3 step 14).
//
// Deliberately minimal: an open SoftAP with a fixed SSID plus one scan. No
// credentials exist anywhere in this module (AGENTS rule 17), no network is
// ever joined, and nothing depends on the internet — the radio's own
// beacon/probe traffic is the TX load the brownout test needs. Phase 1
// replaces this with the real Wi-Fi manager.

#pragma once

#include <cstdint>

#include "esp_err.h"

namespace visioncam {

struct WifiReport {
    bool started = false;       // radio up (SoftAP beaconing)
    uint16_t scan_results = 0;  // networks heard during the proof scan
};

// Brings Wi-Fi up in AP+STA mode with an AP-only configuration (the STA
// interface exists only because scanning requires it) and runs one blocking
// scan: probe requests prove the radio transmits, the result count proves it
// receives. Idempotent — netif/event loop/driver are created on first call.
esp_err_t wifiStart(WifiReport& report);

// Stops the radio (Wi-Fi "off"). Succeeds when already off, so callers can
// pair start/stop freely. The driver stays initialised for the next start.
esp_err_t wifiStop(WifiReport& report);

}  // namespace visioncam
