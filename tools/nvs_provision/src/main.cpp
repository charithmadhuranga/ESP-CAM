// tools/nvs_provision/src/main.cpp — Phase 0 D2 helper (docs/HARDWARE.md §3.7)
//
// Why this exists:
//   R1 (ESP32-CAM_MJPEG2SD) reads its Wi-Fi credentials from the ESP32 NVS
//   Preferences namespace "ESP-CAM_MJPEG" (keys "ST_SSID" / "ST_Pass") when
//   its SD config file holds no SSID — i.e. exactly the first-boot state.
//   Writing those two keys here is the machine equivalent of typing the
//   router credentials into R1's own setup page, and it lets the R1 baseline
//   be measured over HTTP from this workstation.
//
// Clean-room (owner decision D1): this tool shares no R1 code. It only
// writes two documented NVS keys; no R1 source is copied or translated.
//
// Security (AGENTS rule 17): the passphrase is read from the console, never
// logged, never echoed back, and never stored in this firmware image or the
// repository. Only its LENGTH is ever printed. The SSID is not treated as
// secret (R1 logs it itself).
//
// Protocol at 115200 baud, one ASCII line, TAB separated:
//     set\t<ssid>\t<pass>\n     write both keys and commit
//     show\n                    report key lengths (values hidden)
//     join\n                    try joining the Wi-Fi with the stored keys
//                               and print the exact disconnect reason —
//                               the diagnostic for a failed R1 connection
//     vcunset\n / vcunset\tERASE  delete ONLY the firmware's migrated copies
//                               in the "visioncam" namespace (typed
//                               confirmation; the legacy keys above are the
//                               source of truth and are never deleted)

#include <cstdio>
#include <cstring>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

constexpr char kNamespace[] = "ESP-CAM_MJPEG";  // R1's APP_NAME
constexpr char kKeySsid[]    = "ST_SSID";
constexpr char kKeyPass[]    = "ST_Pass";

// VisionCam's own namespace (firmware/include/wifi_credentials.h): the
// firmware migrates the two legacy keys into it on first use. This command
// can delete ONLY those copied keys, so the legacy provisioning above is
// left as the source of truth — never a way to wipe the credentials.
constexpr char kNsVisioncam[] = "visioncam";
constexpr char kVcKeySsid[]   = "wifi_ssid";
constexpr char kVcKeyPass[]   = "wifi_pass";
// R1's buffers: ST_SSID[MAX_HOST_LEN=32], ST_Pass read with MAX_PWD_LEN=64.
constexpr size_t kMaxSsid = 31;
constexpr size_t kMaxPass = 63;
constexpr size_t kLineMax = 192;
constexpr size_t kMaxHostChars = 31;  // R1 ST_SSID buffer (MAX_HOST_LEN-1)

void reportLength(const char* key, size_t len) {
    std::printf("cfgtool: %s present, %u chars\n", key,
                static_cast<unsigned>(len));
}

// nvs_flash_init() fails with NO_FREE_PAGES / NEW_VERSION on a partition
// written by a different NVS version or when full. Erasing is safe here:
// the only other content would be Wi-Fi calibration data, which the radio
// stack regenerates on its next init.
esp_err_t initNvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        std::printf("cfgtool: NVS unusable (%s) — erasing and retrying\n",
                    esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) return err;
        err = nvs_flash_init();
    }
    return err;
}

int splitTabs(char* line, char* fields[], int maxFields) {
    int count = 0;
    char* p = line;
    fields[count++] = p;
    while (*p && count < maxFields) {
        if (*p == '\t') {
            *p = '\0';
            fields[count++] = p + 1;
        }
        ++p;
    }
    return count;
}

void handleSet(char* fields[], int count) {
    if (count != 3) {
        std::printf("cfgtool: usage: set\\t<ssid>\\t<pass>\n");
        return;
    }
    const char* ssid = fields[1];
    const char* pass = fields[2];
    const size_t ssidLen = std::strlen(ssid);
    const size_t passLen = std::strlen(pass);
    if (ssidLen == 0 || ssidLen > kMaxSsid || passLen > kMaxPass) {
        std::printf("cfgtool: rejected (ssid 1..%u chars, pass <=%u chars; "
                    "got ssid %u, pass hidden)\n",
                    static_cast<unsigned>(kMaxSsid),
                    static_cast<unsigned>(kMaxPass),
                    static_cast<unsigned>(ssidLen));
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        std::printf("cfgtool: nvs_open failed: %s\n", esp_err_to_name(err));
        return;
    }

    if ((err = nvs_set_str(handle, kKeySsid, ssid)) != ESP_OK ||
        (err = nvs_set_str(handle, kKeyPass, pass)) != ESP_OK) {
        std::printf("cfgtool: nvs_set_str failed: %s\n", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }
    if ((err = nvs_commit(handle)) != ESP_OK) {
        std::printf("cfgtool: nvs_commit failed: %s\n", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }

    // Read back — but only ever report lengths.
    size_t ssidBack = 0, passBack = 0;
    nvs_get_str(handle, kKeySsid, nullptr, &ssidBack);
    nvs_get_str(handle, kKeyPass, nullptr, &passBack);
    nvs_close(handle);

    std::printf("cfgtool: namespace %s\n", kNamespace);
    std::printf("cfgtool: %s = \"%s\" (%u chars)\n", kKeySsid, ssid,
                static_cast<unsigned>(ssidLen));
    reportLength(kKeyPass, passBack);
    if (ssidBack == ssidLen + 1 && passBack == passLen + 1) {
        std::printf("cfgtool: OK — committed and verified (pass hidden)\n");
    } else {
        std::printf("cfgtool: FAIL — readback mismatch (ssid %u vs %u, "
                    "pass %u vs %u)\n",
                    static_cast<unsigned>(ssidBack),
                    static_cast<unsigned>(ssidLen + 1),
                    static_cast<unsigned>(passBack),
                    static_cast<unsigned>(passLen + 1));
    }
}

// --- delete only the firmware's COPIES of the credentials ----------------
// Why this exists: firmware's wifiCredentialsLoad() migrates the two
// legacy keys into VisionCam's own namespace on first use. Later boots
// then read the migrated copy and the migration path is silent. This
// command deletes ONLY the copies, so a later boot demonstrably takes the
// legacy-fallback path again — the check for that migration.
// The legacy namespace and the Wi-Fi calibration data are never touched,
// and a typed confirmation gates the destructive step (same policy as
// tools/sd_format: nothing happens before ERASE).
bool s_vcunset_armed = false;

void handleVcUnset(char* fields[], int count) {
    if (count != 2 || std::strcmp(fields[1], "ERASE") != 0) {
        // First call (or a wrong confirmation): arm and stop.
        s_vcunset_armed = true;
        std::printf("cfgtool: vcunset deletes %s/%s and %s/%s — the legacy "
                    "keys above are NOT touched\n",
                    kNsVisioncam, kVcKeySsid, kNsVisioncam, kVcKeyPass);
        std::printf("cfgtool: type 'vcunset\\tERASE' to proceed\n");
        return;
    }
    if (!s_vcunset_armed) {
        std::printf("cfgtool: run 'vcunset' once first (armed state required)\n");
        return;
    }
    s_vcunset_armed = false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNsVisioncam, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        std::printf("cfgtool: namespace %s does not exist — nothing to do\n",
                    kNsVisioncam);
        return;
    }
    if (err != ESP_OK) {
        std::printf("cfgtool: nvs_open(%s) failed: %s\n", kNsVisioncam,
                    esp_err_to_name(err));
        return;
    }

    int deleted = 0;
    int absent = 0;
    err = nvs_erase_key(handle, kVcKeySsid);
    if (err == ESP_OK) ++deleted;
    else if (err == ESP_ERR_NVS_NOT_FOUND) ++absent;
    else std::printf("cfgtool: delete %s failed: %s\n", kVcKeySsid,
                     esp_err_to_name(err));
    err = nvs_erase_key(handle, kVcKeyPass);
    if (err == ESP_OK) ++deleted;
    else if (err == ESP_ERR_NVS_NOT_FOUND) ++absent;
    else std::printf("cfgtool: delete %s failed: %s\n", kVcKeyPass,
                     esp_err_to_name(err));
    if ((err = nvs_commit(handle)) != ESP_OK) {
        std::printf("cfgtool: nvs_commit failed: %s\n", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }
    nvs_close(handle);
    std::printf("cfgtool: namespace %s — erased %d key(s), %d already absent\n",
                kNsVisioncam, deleted, absent);
    if (deleted > 0) {
        std::printf("cfgtool: a firmware boot now migrates from %s again\n",
                    kNamespace);
    }
}

void handleShow() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        std::printf("cfgtool: namespace %s not open yet: %s\n", kNamespace,
                    esp_err_to_name(err));
        return;
    }
    size_t ssidLen = 0, passLen = 0;
    const esp_err_t ssidErr = nvs_get_str(handle, kKeySsid, nullptr, &ssidLen);
    const esp_err_t passErr = nvs_get_str(handle, kKeyPass, nullptr, &passLen);
    nvs_close(handle);
    std::printf("cfgtool: namespace %s\n", kNamespace);
    if (ssidErr == ESP_OK) reportLength(kKeySsid, ssidLen - 1);
    else std::printf("cfgtool: %s missing (%s)\n", kKeySsid,
                     esp_err_to_name(ssidErr));
    if (passErr == ESP_OK) reportLength(kKeyPass, passLen - 1);
    else std::printf("cfgtool: %s missing (%s)\n", kKeyPass,
                     esp_err_to_name(passErr));
}

// --- Wi-Fi join diagnostic ---------------------------------------------------
// Reproduces what R1 does (STA connect with the stored keys) but prints the
// driver's raw WIFI_REASON_* code, which R1's log does not expose. Values are
// from esp_wifi_types_generic.h; common ones are commented inline.
EventGroupHandle_t s_wifiEvents;
constexpr int kBitGotIp   = BIT0;
constexpr int kBitDropped = BIT1;

void wifiEventHandler(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* d = static_cast<wifi_event_sta_disconnected_t*>(data);
        std::printf("cfgtool: WIFI_EVENT_STA_DISCONNECTED reason=%u%s\n",
                    static_cast<unsigned>(d->reason),
                    d->reason == 15 ? " (FOUR_WAY_HANDSHAKE_TIMEOUT — usually"
                                      " wrong passphrase)"
                    : d->reason == 205 ? " (CONNECTION_FAIL / HS timeout)"
                    : d->reason == 202 ? " (AUTH_FAIL — AP refused (PMF/WPA3?)"
                    : d->reason == 201 ? " (NO_AP_FOUND)"
                    : d->reason == 200 ? " (BEACON_TIMEOUT)"
                    : d->reason == 203 ? " (ASSOC_FAIL)"
                    : d->reason == 204 ? " (ASSOC_TIMEOUT)"
                    : d->reason == 5   ? " (ASSOC_TOOMANY — client limit?"
                    : d->reason == 2   ? " (AUTH_EXPIRE)"
                    : "");
        xEventGroupSetBits(s_wifiEvents, kBitDropped);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* e = static_cast<ip_event_got_ip_t*>(data);
        std::printf("cfgtool: IP_EVENT_STA_GOT_IP " IPSTR "\n",
                    IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifiEvents, kBitGotIp);
    }
}

void handleJoin() {
    // Keys come from NVS only — the passphrase never transits the console.
    char ssid[kMaxHostChars + 1] = {};
    char pass[64] = {};
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) {
        std::printf("cfgtool: join refused — no keys in NVS\n");
        return;
    }
    size_t sl = sizeof(ssid), pl = sizeof(pass);
    const esp_err_t e1 = nvs_get_str(h, kKeySsid, ssid, &sl);
    const esp_err_t e2 = nvs_get_str(h, kKeyPass, pass, &pl);
    nvs_close(h);
    if (e1 != ESP_OK || e2 != ESP_OK || ssid[0] == '\0') {
        std::printf("cfgtool: join refused — keys missing (%s / %s)\n",
                    esp_err_to_name(e1), esp_err_to_name(e2));
        return;
    }

    s_wifiEvents = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t initCfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&initCfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, nullptr, nullptr));

    std::printf("cfgtool: join \"%s\" (pass hidden)\n", ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), ssid,
                 sizeof(cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(cfg.sta.password), pass,
                 sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    uint8_t mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    std::printf("cfgtool: station MAC %02x:%02x:%02x:%02x:%02x:%02x "
                "(compare with the hotspot's client list)\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_ERROR_CHECK(esp_wifi_connect());

    const EventBits_t bits =
        xEventGroupWaitBits(s_wifiEvents, kBitGotIp | kBitDropped, pdTRUE,
                            pdFALSE, pdMS_TO_TICKS(20000));
    if (bits & kBitGotIp) {
        std::printf("cfgtool: JOIN OK\n");
    } else if (bits & kBitDropped) {
        std::printf("cfgtool: JOIN FAIL (see reason above)\n");
        esp_wifi_connect();  // one retry, more reason evidence
        const EventBits_t retry =
            xEventGroupWaitBits(s_wifiEvents, kBitGotIp | kBitDropped,
                                pdTRUE, pdFALSE, pdMS_TO_TICKS(20000));
        std::printf(retry & kBitGotIp ? "cfgtool: JOIN OK on retry\n"
                                      : "cfgtool: JOIN FAIL on retry\n");
    } else {
        std::printf("cfgtool: JOIN TIMEOUT (no event in 20 s)\n");
    }
    esp_wifi_stop();
    esp_wifi_deinit();
}

// --- console input -----------------------------------------------------------
// fgets()/stdin returns EOF immediately on this IDF configuration (the simple
// VFS read reports "no data" as end-of-file), so the UART0 driver is used
// explicitly and lines are assembled one byte at a time. If the console
// already installed the driver, read from it as-is.
bool consoleReady = false;

void consoleInit() {
    if (uart_is_driver_installed(UART_NUM_0)) {
        consoleReady = true;
        std::printf("cfgtool: console driver already installed\n");
        return;
    }
    uart_config_t cfg = {};
    cfg.baud_rate   = 115200;
    cfg.data_bits   = UART_DATA_8_BITS;
    cfg.parity      = UART_PARITY_DISABLE;
    cfg.stop_bits   = UART_STOP_BITS_1;
    cfg.flow_ctrl   = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk  = UART_SCLK_DEFAULT;
    if (uart_param_config(UART_NUM_0, &cfg) != ESP_OK) return;
    if (uart_driver_install(UART_NUM_0, 1024, 0, 0, nullptr, 0) != ESP_OK) {
        return;
    }
    consoleReady = true;
    std::printf("cfgtool: console driver installed\n");
}

// Blocks until a line arrives. Returns false for an oversized line (its
// content is discarded up to the newline; buf[0] is then '\0').
bool readLine(char* buf, size_t max) {
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

}  // namespace

extern "C" void app_main(void) {
    std::printf("cfgtool: NVS Wi-Fi provisioner ready (115200)\n");
    const esp_err_t err = initNvs();
    if (err != ESP_OK) {
        std::printf("cfgtool: nvs_flash_init failed: %s — aborting\n",
                    esp_err_to_name(err));
        return;
    }
    handleShow();
    consoleInit();
    if (!consoleReady) {
        std::printf("cfgtool: console init failed — aborting\n");
        return;
    }

    char line[kLineMax];
    while (true) {
        if (!readLine(line, sizeof(line))) continue;
        const size_t len = std::strlen(line);
        if (len == 0) continue;

        char* fields[4] = {};
        const int count = splitTabs(line, fields, 4);
        if (std::strcmp(fields[0], "set") == 0) {
            handleSet(fields, count);
        } else if (std::strcmp(fields[0], "show") == 0) {
            handleShow();
        } else if (std::strcmp(fields[0], "join") == 0) {
            handleJoin();
        } else if (std::strcmp(fields[0], "vcunset") == 0) {
            handleVcUnset(fields, count);
        } else {
            // Never echo the raw line: it may contain a passphrase.
            std::printf("cfgtool: unknown command (expected 'set', 'show', "
                        "'join'), %u chars ignored\n",
                        static_cast<unsigned>(len));
        }
    }
}
