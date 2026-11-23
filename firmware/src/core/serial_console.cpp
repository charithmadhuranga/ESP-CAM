// serial_console.cpp — UART0 provisioning console (see serial_console.h).

#include "serial_console.h"

#include <cstdio>
#include <cstring>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tasks.h"
#include "wifi_credentials.h"

namespace visioncam {
namespace {

const char* TAG = "vc_console";

constexpr uart_port_t kPort = UART_NUM_0;
constexpr int kRxBufferBytes = 256;  // driver ring buffer; > 128 B HW FIFO

// Overwrites a buffer that held a passphrase. volatile keeps the compiler
// from eliding the store as dead.
void wipe(char* p, size_t n) {
    volatile char* v = p;
    while (n-- > 0) *v++ = 0;
}

void handleWifi(char* args) {
    // args = "<ssid>\t<pass>" (pass may be empty for an open network).
    char* tab = std::strchr(args, '\t');
    if (tab == nullptr) {
        std::printf("vc_console: ERR usage: wifi<TAB>ssid<TAB>pass\n");
        return;
    }
    *tab = '\0';
    const char* ssid = args;
    const char* pass = tab + 1;
    const unsigned ssid_len = static_cast<unsigned>(std::strlen(ssid));
    const unsigned pass_len = static_cast<unsigned>(std::strlen(pass));

    const esp_err_t err = wifiCredentialsSave(ssid, pass);
    if (err == ESP_ERR_INVALID_ARG) {
        std::printf("vc_console: ERR invalid credentials (ssid %u B, pass %u B; "
                    "need ssid 1-32 B, pass empty or 8-63 B)\n",
                    ssid_len, pass_len);
        return;
    }
    if (err != ESP_OK) {
        std::printf("vc_console: ERR save failed: %s\n", esp_err_to_name(err));
        return;
    }
    std::printf("vc_console: OK wifi credentials saved (ssid %u B, pass %u B) "
                "- rebooting to join\n",
                ssid_len, pass_len);
    std::fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));  // let the reply drain from the TX FIFO
    esp_restart();
}

void handleLine(char* line) {
    if (std::strncmp(line, "wifi\t", 5) == 0) {
        handleWifi(line + 5);
    } else if (std::strcmp(line, "help") == 0) {
        std::printf("vc_console: commands: wifi<TAB>ssid<TAB>pass | help\n");
    } else if (line[0] != '\0') {
        // Never echo: an unknown line could still contain a passphrase.
        std::printf("vc_console: ERR unknown command (%u B, not shown)\n",
                    static_cast<unsigned>(std::strlen(line)));
    }
}

void consoleTask(void*) {
    char line[kSerialConsoleLineMax];
    size_t len = 0;
    bool overflow = false;
    for (;;) {
        uint8_t ch = 0;
        if (uart_read_bytes(kPort, &ch, 1, portMAX_DELAY) != 1) continue;
        if (ch == '\r') continue;
        if (ch != '\n') {
            if (len + 1 < sizeof(line)) {
                line[len++] = static_cast<char>(ch);
            } else {
                overflow = true;  // keep draining until end of line
            }
            continue;
        }
        if (overflow) {
            std::printf("vc_console: ERR line longer than %u B - discarded\n",
                        static_cast<unsigned>(sizeof(line) - 1));
        } else {
            line[len] = '\0';
            handleLine(line);
        }
        wipe(line, sizeof(line));
        len = 0;
        overflow = false;
    }
}

}  // namespace

esp_err_t serialConsoleStart() {
    // TX stays on the non-driver VFS path used by esp_log/printf; only RX
    // goes through the driver's ring buffer.
    esp_err_t err = ESP_OK;
    if (!uart_is_driver_installed(kPort)) {
        err = uart_driver_install(kPort, kRxBufferBytes, 0, 0, nullptr, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart driver install failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (xTaskCreate(consoleTask, "vc_console", VC_STACK_CONSOLE, nullptr,
                    VC_PRIO_CONSOLE, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "console task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "serial console ready (type 'help')");
    return ESP_OK;
}

}  // namespace visioncam
