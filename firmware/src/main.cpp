// Phase 0 smoke firmware — plan 01-phase-0-baseline.md, task 0.3.
//
//   step 10  reset reason, PSRAM, heap reporting
//   step 11  camera bring-up        (visioncam::cameraBringup)
//   step 12  SD 1-bit mount + write/read/delete  (visioncam::sdMount/sdSelfTest)
//   step 13  GPIO4 / flash-lamp behaviour with the SD mounted
//
// Steps 11-13 compile and link here; they can only RUN on the owner's board.
// No serial device is attached to this machine, so every runtime result stays
// UNVERIFIED until a serial log exists (AGENTS rule 26).

#include <cstdio>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_profile.h"
#include "camera_bringup.h"
#include "sd_bringup.h"

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

static void logHeap(const char* where) {
    ESP_LOGI(TAG, "heap[%s] free: %u B, min ever: %u B, largest block: %u B",
             where,
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(esp_get_minimum_free_heap_size()),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
    if (esp_psram_get_size() > 0) {
        ESP_LOGI(TAG, "heap[%s] PSRAM free: %u B, largest PSRAM block: %u B",
                 where,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(
                     heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " VisionCam Pro — Phase 0 smoke test");
    ESP_LOGI(TAG, "================================================");

    // --- step 10: reset cause (SPEC 2.4) -----------------------------------
    const esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "reset reason: %s", resetReasonName(reason));
    if (reason == ESP_RST_BROWNOUT) {
        ESP_LOGW(TAG, "brownout detected — check the 5 V supply under Wi-Fi TX load");
    }

    // --- step 10: PSRAM (decides the init frame size, SPEC 4.1.1) ----------
    const size_t psramSize = esp_psram_get_size();
    if (psramSize == 0) {
        ESP_LOGE(TAG, "PSRAM NOT DETECTED — camera init will fail (frames live in PSRAM)");
    } else {
        ESP_LOGI(TAG, "PSRAM: %u bytes (%.1f MB)",
                 static_cast<unsigned>(psramSize), psramSize / (1024.0 * 1024.0));
    }

    logHeap("boot");

    ESP_LOGI(TAG, "board: AI-Thinker ESP32-CAM, flash LED GPIO%d, SD CLK/CMD/D0 = %d/%d/%d",
             BOARD_FLASH_LED, BOARD_SD_CLK, BOARD_SD_CMD, BOARD_SD_D0);

    // --- step 11: camera bring-up ------------------------------------------
    visioncam::CameraReport cam;
    const esp_err_t camErr = visioncam::cameraBringup(cam);
    if (camErr != ESP_OK) {
        ESP_LOGE(TAG, "CAMERA FAIL: %s (attempts=%u, initialised=%d, pid=0x%04x)",
                 esp_err_to_name(camErr), cam.init_attempts,
                 cam.initialised ? 1 : 0, cam.sensor_pid);
    } else {
        ESP_LOGI(TAG, "CAMERA OK: pid=0x%04x, attempts=%u, warmup=%u, jpeg=%u B (%ux%u)",
                 cam.sensor_pid, cam.init_attempts, cam.warmup_discarded,
                 static_cast<unsigned>(cam.jpeg_bytes), cam.jpeg_width, cam.jpeg_height);
    }

    logHeap("after camera");

    // --- step 12 + 13: SD 1-bit mount, then the GPIO4 test -----------------
    visioncam::SdReport sd;
    const esp_err_t sdErr = visioncam::sdMount(sd);
    if (sdErr != ESP_OK) {
        // Not fatal: everything except storage keeps working (SPEC 2.3).
        ESP_LOGW(TAG, "SD FAIL: %s — continuing without storage", esp_err_to_name(sdErr));
    } else {
        const esp_err_t testErr = visioncam::sdSelfTest();
        sd.self_test_ok = (testErr == ESP_OK);
        if (!sd.self_test_ok) {
            ESP_LOGE(TAG, "SD self-test FAILED: %s", esp_err_to_name(testErr));
        }
        ESP_LOGI(TAG, "SD OK: width=%u bit, max %u kHz, self-test=%s",
                 static_cast<unsigned>(sd.bus_width),
                 static_cast<unsigned>(sd.max_freq_khz),
                 sd.self_test_ok ? "pass" : "FAIL");
        // step 13's real verdict (lamp on/off) is observed on the board and
        // written into docs/HARDWARE.md — the firmware can only report that it
        // drove the pin low.
        ESP_LOGI(TAG, "step 13: GPIO%d driven low — observer must confirm the "
                      "lamp is OFF", BOARD_FLASH_LED);
    }

    logHeap("after SD");

    // --- verdict -----------------------------------------------------------
    ESP_LOGI(TAG, "------------------------------------------------");
    ESP_LOGI(TAG, " camera: %s | sd: %s | psram: %s",
             camErr == ESP_OK ? "OK" : "FAIL",
             sdErr == ESP_OK ? (sd.self_test_ok ? "OK" : "SELFTEST-FAIL") : "FAIL",
             psramSize > 0 ? "OK" : "MISSING");
    ESP_LOGI(TAG, " next: flash this build and fill docs/HARDWARE.md section 5");
    ESP_LOGI(TAG, "================================================");

    // Keep the task alive so heap minima accumulate across the idle period.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
