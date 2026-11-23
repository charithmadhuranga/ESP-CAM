// serial_console.h — UART0 provisioning console.
//
// Lets a workstation set the Wi-Fi credentials over the USB serial link
// (ESP32-CAM-MB) without joining the camera's SoftAP. Physical UART access
// already implies full flash access, so this adds no new trust boundary.
//
// Protocol: 115200 baud, one ASCII line per command, fields TAB-separated
// (an SSID may contain spaces or quotes, never a TAB):
//
//   wifi<TAB><ssid><TAB><pass>\n   validate + persist, then reboot to join
//   help\n                         list commands
//
// Secret-handling contract (AGENTS rule 17): the line is never echoed and
// every reply reports LENGTHS only. The line buffer is fixed-size
// (kSerialConsoleLineMax); an over-long line is discarded whole, not
// truncated (AGENTS rule 13). Validation is wifiCredentialsSave()'s own
// (SSID 1..32 B, passphrase empty or 8..63 B).

#pragma once

#include <cstddef>

#include "esp_err.h"

namespace visioncam {

// "wifi\t" + 32 B SSID + "\t" + 63 B passphrase + slack.
constexpr size_t kSerialConsoleLineMax = 128;

// Installs the UART0 RX driver and starts the low-priority console task.
// Failure is not fatal: every other subsystem works without the console.
esp_err_t serialConsoleStart();

}  // namespace visioncam
