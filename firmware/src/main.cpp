// Phase 0 smoke test — proves the toolchain, PSRAM and heap reporting work
// before any camera or SD code exists (plan 01-phase-0-baseline.md, task 0.3
// step 10).
//
// This file is deliberately small: it must build and flash on a bare board
// with no camera, SD card or Wi-Fi configuration present.

#include <cstdio>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_profile.h"

static const char* TAG = "vc_smoke";

// esp_reset_reason() has no dedicated accessor for a human-readable string,
// so map the enum explicitly rather than printing raw integers.
static const char* resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " VisionCam Pro — Phase 0 smoke test");
    ESP_LOGI(TAG, "================================================");

    // --- reset cause (SPEC 2.4: detect and log brownout/reset causes) -----
    const esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "reset reason: %s", resetReasonName(reason));
    if (reason == ESP_RST_BROWNOUT) {
        ESP_LOGW(TAG, "brownout detected — check the 5 V supply under Wi-Fi TX load");
    }

    // --- PSRAM (decides the initial frame size in SPEC 4.1.1) -------------
    // Symbol verified against the pinned ESP-IDF before this is trusted;
    // see docs/adr/0001-toolchain.md.
    const size_t psramSize = esp_psram_get_size();
    if (psramSize == 0) {
        ESP_LOGE(TAG, "PSRAM NOT DETECTED — camera init will fail (frames live in PSRAM)");
    } else {
        ESP_LOGI(TAG, "PSRAM: %u bytes (%.1f MB)",
                 static_cast<unsigned>(psramSize), psramSize / (1024.0 * 1024.0));
    }

    // --- heap -------------------------------------------------------------
    const size_t freeHeap   = esp_get_free_heap_size();
    const size_t minHeap    = esp_get_minimum_free_heap_size();
    const size_t largestBlk = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "heap free: %u B, min ever: %u B, largest block: %u B",
             static_cast<unsigned>(freeHeap),
             static_cast<unsigned>(minHeap),
             static_cast<unsigned>(largestBlk));

    if (psramSize > 0) {
        ESP_LOGI(TAG, "PSRAM free: %u B, largest PSRAM block: %u B",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    }

    // --- board profile sanity (pins must come from board_profile.h) -------
    ESP_LOGI(TAG, "board: AI-Thinker ESP32-CAM, flash LED GPIO%d, SD CLK/CMD/D0 = %d/%d/%d",
             BOARD_FLASH_LED, BOARD_SD_CLK, BOARD_SD_CMD, BOARD_SD_D0);

    ESP_LOGI(TAG, "smoke test OK — next: camera bring-up (task 0.3 step 11)");
}
