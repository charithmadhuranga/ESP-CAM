// core_init.cpp — see core_init.h.

#include "core_init.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "event_bus.h"
#include "resource_monitor.h"
#include "ring_log.h"

namespace visioncam {
namespace {

const char* TAG = "vc_core";

esp_err_t nvsInit() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // First boot on a fresh partition table: erase once and retry. This
        // is the only erase path in the firmware — nothing ever formats or
        // wipes media storage (SPEC 2.3; NVS is not media storage).
        ESP_LOGW(TAG, "nvs: %s — erasing partition", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) return err;
        err = nvs_flash_init();
    }
    return err;
}

}  // namespace

esp_err_t coreInit() {
    // 1. RTC ring log FIRST: everything after this point can be evidence.
    ringLogInit();
    ringLogDump();   // previous session + boot marker, before other tasks

    // 2. NVS: config store (SPEC 7) and Wi-Fi calibration both need it.
    esp_err_t err = nvsInit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
        VC_RING_ERROR(TAG, "nvs init failed: %s", esp_err_to_name(err));
        return err;  // config and Wi-Fi cannot work; let app_main decide
    }

    // 3. Event bus (SPEC 8.3).
    err = eventBusInit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event bus init failed: %s", esp_err_to_name(err));
        VC_RING_ERROR(TAG, "event bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Mirror every event into the RTC ring log: a crash must not take the
    // event trail with it (SPEC 8.4.1). Contract-compliant handler: quick,
    // non-blocking, no publish loop (event_bus.h).
    err = eventBusSubscribe([](const Event& ev) {
        VC_RING_INFO("event", "%s value=%u code=%d",
                     eventTypeName(ev.type),
                     static_cast<unsigned>(ev.value),
                     static_cast<int>(ev.code));
    });
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event subscriber registration failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    // 4. Resource monitor (SPEC 8.4).
    err = resourceMonitorInit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "resource monitor init failed: %s", esp_err_to_name(err));
        VC_RING_ERROR(TAG, "resource monitor init failed: %s",
                      esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "core services up (boot %u, abnormal %u)",
             static_cast<unsigned>(ringLogBootCount()),
             static_cast<unsigned>(ringLogAbnormalBoots()));
    return ESP_OK;
}

}  // namespace visioncam
