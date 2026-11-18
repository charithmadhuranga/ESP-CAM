// tools/nvs_reset_auth — one-shot factory-credential reset.
//
// Erases ONLY the stored admin password blob (`creds`) from the
// `visioncam_auth` namespace, so the next firmware boot falls back to the
// SPEC 4.2 factory default (admin/admin). Owner-authorised 2026-11-18 after
// the Phase 1 change-password test left a password that was never recorded
// (AGENTS rule 17 keeps secrets out of docs).
//
// Safety rules this tool obeys:
//  - typed "ERASE" gate (same pattern as tools/sd_format);
//  - it NEVER erases the NVS partition — only one key. A partition erase
//    would destroy the Wi-Fi credentials too, which is exactly the data-loss
//    incident recorded in docs/REFERENCE_ANALYSIS.md;
//  - if nvs_flash_init() reports a damaged partition it aborts instead of
//    "repairing" it, and says so;
//  - it prints the nvs offset/size it booted with, so the log proves the view
//    matches the firmware's 0x9000 / 0x17000 (a mismatched view is the
//    corruption hazard above);
//  - it prints key presence and length only, never secret material
//    (AGENTS rule 17).
//
// Workflow: flash this tool (it occupies the firmware's ota_0 slot), type
// ERASE at the prompt, then flash the firmware back. Never a release build.

#include <cstdio>
#include <cstring>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

const char* TAG = "nvsreset";

// Must match firmware/src/auth/auth.cpp exactly (that file is the authority).
constexpr char kNsAuth[]   = "visioncam_auth";
constexpr char kKeyCreds[] = "creds";

constexpr char   kEraseToken[] = "ERASE";
constexpr size_t kLineMax      = 128;

// Probe buffer for reading key length without ever printing the value.
constexpr size_t kProbeCap = 512;

// --- console input -----------------------------------------------------------
// fgets()/stdin returns EOF immediately on this IDF configuration (the simple
// VFS read reports "no data" as end-of-file), so the UART0 driver is used
// explicitly and lines are assembled one byte at a time — the pattern proven
// by tools/sd_format and tools/nvs_provision.
bool s_consoleReady = false;

void consoleInit() {
    if (uart_is_driver_installed(UART_NUM_0)) {
        s_consoleReady = true;
        return;
    }
    uart_config_t cfg = {};
    cfg.baud_rate  = 115200;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    if (uart_param_config(UART_NUM_0, &cfg) != ESP_OK) return;
    if (uart_driver_install(UART_NUM_0, 1024, 0, 0, nullptr, 0) != ESP_OK) {
        return;
    }
    s_consoleReady = true;
}

// Blocks until a line arrives. Returns false for an oversized line (content
// discarded up to the newline; buf[0] is then '\0').
bool readLine(char* buf, size_t max) {
    if (!s_consoleReady) return false;
    size_t i = 0;
    bool overflow = false;
    for (;;) {
        uint8_t c = 0;
        const int n = uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY);
        if (n != 1) continue;
        if (c == '\n') {
            if (overflow) {
                buf[0] = '\0';
                return false;
            }
            buf[i] = '\0';
            return true;
        }
        if (c == '\r') continue;
        if (overflow) continue;
        if (i + 1 < max) {
            buf[i++] = static_cast<char>(c);
        } else {
            overflow = true;
        }
    }
}

// Reports whether the creds key exists — length only, never the bytes.
// present: 0 = absent, 1 = present, 2 = present but larger than the probe.
int credsPresence(size_t* out_len) {
    if (out_len) *out_len = 0;
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kNsAuth, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return 0;   // namespace itself absent
    if (err != ESP_OK) {
        std::printf("nvsreset: nvs_open(%s) failed: %s\n", kNsAuth,
                    esp_err_to_name(err));
        return -1;
    }
    static char probe[kProbeCap];
    size_t n = sizeof(probe);
    const esp_err_t r = nvs_get_str(h, kKeyCreds, probe, &n);
    nvs_close(h);
    std::memset(probe, 0, sizeof(probe));   // secret material never lingers
    if (r == ESP_ERR_NVS_NOT_FOUND) return 0;
    if (r == ESP_ERR_NVS_INVALID_LENGTH) {
        if (out_len) *out_len = n;
        return 2;
    }
    if (r != ESP_OK) {
        std::printf("nvsreset: probe failed: %s\n", esp_err_to_name(r));
        return -1;
    }
    if (out_len) *out_len = n;
    return 1;
}

}  // namespace

extern "C" void app_main(void) {
    consoleInit();
    std::printf("\n==== tools/nvs_reset_auth — factory-credential reset ====\n");
    std::printf("Erases ONE key: ns=%s key=%s (the stored admin password).\n",
                kNsAuth, kKeyCreds);
    std::printf("Wi-Fi credentials and all other NVS data are untouched;\n");
    std::printf("this tool never erases the partition.\n\n");

    // Prove which NVS view we booted with (must equal the firmware's).
    const esp_partition_t* nvs =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 ESP_PARTITION_SUBTYPE_DATA_NVS, nullptr);
    if (nvs == nullptr) {
        std::printf("nvsreset: ABORT — no nvs partition in the active table\n");
        return;
    }
    std::printf("nvs view: offset=0x%lx size=%lu B (%lu KB)\n",
                static_cast<unsigned long>(nvs->address),
                static_cast<unsigned long>(nvs->size),
                static_cast<unsigned long>(nvs->size / 1024));
    const bool view_matches = (nvs->address == 0x9000) && (nvs->size == 0x17000);
    std::printf("nvs view matches firmware (0x9000 / 0x17000): %s\n",
                view_matches ? "YES" : "NO — ABORTING");
    if (!view_matches) {
        std::printf("nvsreset: ABORT — a mismatched nvs view is how this "
                    "project corrupted NVS once before; refusing to write.\n");
        return;
    }

    // NEVER call nvs_flash_erase here: ESP_ERR_NVS_NO_FREE_PAGES means the
    // partition is damaged, and repairing it would take the Wi-Fi
    // credentials with it. Report and stop instead.
    const esp_err_t ierr = nvs_flash_init();
    if (ierr != ESP_OK) {
        std::printf("nvsreset: ABORT — nvs_flash_init(): %s\n"
                    "           The firmware must diagnose this; this tool "
                    "will not erase NVS.\n",
                    esp_err_to_name(ierr));
        return;
    }

    size_t len = 0;
    const int present = credsPresence(&len);
    if (present < 0) {
        std::printf("nvsreset: ABORT — could not read the current state\n");
        return;
    }
    if (present == 0) {
        std::printf("\ncreds key: ABSENT — already at the factory default "
                    "(admin/admin). Nothing to do.\n");
        return;
    }
    std::printf("\ncreds key: PRESENT (%s, %u B) — a password is set\n",
                present == 2 ? "larger than probe" : "readable",
                static_cast<unsigned>(len));
    std::printf("Wi-Fi credentials: NOT TOUCHED (single-key erase only)\n");

    std::printf("\nType %s and press Enter to erase the stored password\n"
                "(anything else aborts): ",
                kEraseToken);
    std::fflush(stdout);

    char line[kLineMax] = {};
    for (;;) {
        if (!readLine(line, sizeof(line))) {
            std::printf("\nnvsreset: no console input — aborting (nothing "
                        "written)\n");
            return;
        }
        if (std::strcmp(line, kEraseToken) != 0) {
            std::printf("nvsreset: %s != %s — aborting (nothing written)\n",
                        line[0] ? line : "(empty)", kEraseToken);
            return;
        }
        break;
    }

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kNsAuth, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        std::printf("nvsreset: nvs_open(RW) failed: %s — nothing written\n",
                    esp_err_to_name(err));
        return;
    }
    err = nvs_erase_key(h, kKeyCreds);
    if (err != ESP_OK) {
        nvs_close(h);
        std::printf("nvsreset: nvs_erase_key failed: %s — nothing committed\n",
                    esp_err_to_name(err));
        return;
    }
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        std::printf("nvsreset: nvs_commit failed: %s\n", esp_err_to_name(err));
        return;
    }

    // Verify from a fresh read before claiming success.
    size_t after = 0;
    const int still = credsPresence(&after);
    if (still == 0) {
        std::printf("\nAUTH RESET OK — creds key absent after re-read.\n"
                    "Reflash the firmware; the next boot accepts admin/admin.\n"
                    "Change it immediately via POST /auth/complete-first-login.\n");
    } else {
        std::printf("\nnvsreset: VERIFY FAILED — creds still present "
                    "(state %d). Do NOT assume the reset worked.\n", still);
    }
    ESP_LOGI(TAG, "reset attempt finished (state=%d)", still);
}
