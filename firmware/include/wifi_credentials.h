// wifi_credentials.h — Wi-Fi credentials from NVS (plan 02 task 1.1.2).
//
// Secret-handling contract (AGENTS rule 17):
//   - values are written to the CALLER's buffers only, never to a log,
//     an event payload, an API response or a global;
//   - every log line reports LENGTHS only;
//   - the passphrase is a separate NVS key from the SSID and lives in the
//     same secret store SPI/config later (SPEC 7: secrets in a dedicated
//     namespace, GET returns `*_configured` booleans instead).
//
// Storage (SPEC 7: NVS authoritative, versioned, explicit migration):
// namespace "visioncam", keys wifi_ssid / wifi_pass   — our active config
//   namespace "ESP-CAM_MJPEG", keys ST_SSID / ST_Pass   — the Phase 0
//                                                          provisioning path
//                                                          (tools/nvs_provision,
//                                                          docs/HARDWARE.md 3.7),
//                                                          kept as a one-time
//                                                          MIGRATION FALLBACK so
//                                                          a board provisioned
//                                                          during the R1 baseline
//                                                          needs no re-flow.
//
// The fallback is deliberately temporary: it is consulted only while the
// VisionCam namespace holds no SSID, and a value read from it is copied
// into the VisionCam namespace (best effort) on first use. When task 1.2
// lands, the config store becomes the only writer and this migration path
// is deleted — do not extend it.
//
// Limits: SSID 1..32 bytes, passphrase 0..63 bytes (the ESP32 Wi-Fi
// driver's own maxima — a longer value fails validation, it is not
// truncated).

#pragma once

#include <cstddef>

#include "esp_err.h"

namespace visioncam {

// Loads the stored credentials. On success *found is true and
// *ssid_len/*pass_len are the real lengths (both buffers NUL-terminated).
// When no credentials exist *found is false and ESP_OK is returned —
// "unconfigured" is a normal state, not an error.
// Never logs any value; lengths only.
esp_err_t wifiCredentialsLoad(char* ssid, size_t ssid_cap,
                              char* pass, size_t pass_cap,
                              size_t& ssid_len, size_t& pass_len,
                              bool& found);

// Persists a validated network configuration. Empty passphrase is allowed
// for open networks; WPA passphrases must be 8..63 bytes.
esp_err_t wifiCredentialsSave(const char* ssid, const char* pass);

// Stages a verified network for the next software reboot. The caller must
// reboot before losing power; application to NVS happens before camera init.
esp_err_t wifiCredentialsStageForBoot(const char* ssid, const char* pass);
esp_err_t wifiCredentialsApplyStaged(bool& applied);

}  // namespace visioncam