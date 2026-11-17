// core_init.h — the boot order for the core services (plan 02 task 1.1.1).
//
// app_main calls this FIRST, before any subsystem, for one reason: the RTC
// ring log must read the previous session out of RTC memory before anything
// in this boot can reset the chip and destroy that evidence.
//
// Order inside coreInit():
//   1. ringLogInit()            RTC log + crash-loop report (SPEC 8.4.1)
//   2. nvs_flash_init()         config + Wi-Fi calibration storage
//   3. eventBusInit()           typed bounded queue + dispatcher (SPEC 8.3)
//   4. resourceMonitorInit()    metrics + heap thresholds (SPEC 8.4)
//
// After coreInit() returns, app_main brings up the subsystems — network
// FIRST, then camera, then the web server. Network leads because
// esp_wifi_init() reads NVS with the flash cache disabled and that window
// must not overlap the camera DMA ISR (docs/REFERENCE_ANALYSIS.md,
// "Camera driver ISR overflow warning vs flash-cache-off windows");
// nvs_flash_init() above already runs before any ISR exists.
// ringLogMarkHealthy() is called at the health point: end of the boot
// sequence while the smoke test owns app_main (plan 02 task 1.1.1),
// "camera + HTTP serving" once web/ lands (task 1.2). Wi-Fi stays
// optional (SPEC 9 / local-first promise).

#pragma once

#include "esp_err.h"

namespace visioncam {

esp_err_t coreInit();

}  // namespace visioncam
