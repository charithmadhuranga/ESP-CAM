// Phase 0 smoke firmware — plan 01-phase-0-baseline.md, task 0.3.
//
//   step 10  reset reason, PSRAM, heap reporting
//   step 11  camera bring-up        (visioncam::cameraBringup)
//   step 12  SD 1-bit mount + write/read/delete  (visioncam::sdMount/sdSelfTest)
//   step 13  GPIO4 / flash-lamp behaviour with the SD mounted
//   step 14  camera + SD together with Wi-Fi off, then Wi-Fi on
//
// Steps 11-14 compile and link here; they run on the owner's board. Every
// runtime result is recorded in docs/HARDWARE.md section 5 (AGENTS rule 26).

#include <cstdio>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_profile.h"
#include "camera_bringup.h"
#include "core_init.h"
#include "event_bus.h"
#include "network_manager.h"
#include "resource_monitor.h"
#include "ring_log.h"
#include "sd_bringup.h"
#include "wifi_bringup.h"

static const char* TAG = "vc_smoke";

// Reset-reason spelling is shared (visioncam::resetReasonName in
// resource_monitor.h) so the banner, ring log and metrics never diverge.

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

// One camera grab + one SD write/read/delete — the "camera and SD running
// together" probe of plan 0.3 step 14. Frame access stays in camera_bringup
// (AGENTS rule 9) and file access in sd_bringup (rule 10); this helper only
// orchestrates the two.
static bool togetherCycle(const char* phase) {
    size_t bytes = 0;
    const bool cam_ok = visioncam::cameraGrabProbe(&bytes);
    const bool sd_ok = visioncam::sdSelfTest() == ESP_OK;
    ESP_LOGI(TAG, "step 14 [%s]: camera=%s (%u B), sd=%s",
             phase, cam_ok ? "OK" : "FAIL", static_cast<unsigned>(bytes),
             sd_ok ? "OK" : "FAIL");
    return cam_ok && sd_ok;
}

extern "C" void app_main(void) {
    // The default log level is WARN (sdkconfig.defaults), but Phase 0 step 10
    // requires the banner, PSRAM and heap figures as serial evidence. Raise
    // only our own smoke tags back to INFO at runtime; everything else stays
    // quiet. This only works because CONFIG_LOG_MAXIMUM_LEVEL is INFO — at
    // compile time LOG_LOCAL_LEVEL would otherwise discard these ESP_LOGI
    // calls before esp_log_level_set could see them.
    esp_log_level_set("vc_smoke", ESP_LOG_INFO);
    esp_log_level_set("vc_cam", ESP_LOG_INFO);
    esp_log_level_set("vc_sd", ESP_LOG_INFO);
    esp_log_level_set("vc_core", ESP_LOG_INFO);
    esp_log_level_set("vc_resmon", ESP_LOG_INFO);
    esp_log_level_set("vc_net", ESP_LOG_INFO);
    esp_log_level_set("vc_wificred", ESP_LOG_INFO);

    // --- core services (plan 02, task 1.1.1) ------------------------------
    // FIRST: the RTC ring log must read the previous session before this
    // boot can reset the chip (core_init.h documents the full order).
    const esp_err_t coreErr = visioncam::coreInit();
    if (coreErr != ESP_OK) {
        ESP_LOGE(TAG, "CORE SERVICES FAILED: %s — continuing degraded",
                 esp_err_to_name(coreErr));
    }

    // --- network manager FIRST, before the camera (plan 02 task 1.1.2) ---
    // The radio must be up (or deliberately left off) BEFORE the camera is
    // initialised: esp_wifi_start() initialises the radio and reads NVS with
    // the flash cache disabled, and that window must never overlap the
    // camera DMA ISR (docs/REFERENCE_ANALYSIS.md, "Camera driver ISR
    // overflow warning vs flash-cache-off windows" — reproduced as three
    // byte-identical panics, phase1-core-boot-03.log, when Wi-Fi was
    // started while the camera streamed).
    //
    // With no credentials the manager leaves the driver completely
    // untouched and Wi-FiState::Off is the operating mode — local-first
    // (SPEC 9). The Phase 0 SoftAP then still drives the radio below for
    // the TX-load probe.
    const esp_err_t netErr = visioncam::networkManagerInit();
    if (netErr != ESP_OK) {
        ESP_LOGE(TAG, "NETWORK init failed: %s — continuing without Wi-Fi",
                 esp_err_to_name(netErr));
        visioncam::eventBusPublish(visioncam::EventType::WifiDisconnected,
                                   static_cast<uint16_t>(netErr & 0xFF));
    } else {
        const visioncam::WifiStatus ws = visioncam::networkManagerStatus();
        ESP_LOGI(TAG, "network: %s (configured=%d radio_up=%d)",
                 visioncam::wifiStateName(ws.state), ws.configured ? 1 : 0,
                 ws.radio_up ? 1 : 0);
    }

    // Evidence line for the resource monitor: live heap + the monitor's
    // task table (stack high-water marks, worst first).
    visioncam::ResourceSnapshot rs;
    visioncam::resourceMonitorSnapshot(rs);
    ESP_LOGI(TAG, "resource snapshot: free=%u internal=%u min=%u largest=%u "
                  "psram_free=%u tasks=%u boot=%u abnormal=%u",
             static_cast<unsigned>(rs.free_heap),
             static_cast<unsigned>(rs.internal_free),
             static_cast<unsigned>(rs.min_free_heap),
             static_cast<unsigned>(rs.largest_block),
             static_cast<unsigned>(rs.psram_free),
             static_cast<unsigned>(rs.task_count),
             static_cast<unsigned>(rs.boot_count),
             static_cast<unsigned>(rs.abnormal_boots));
    ESP_LOGI(TAG, "task table (worst stack first):");
    for (uint16_t i = 0; i < rs.tasks_reported; ++i) {
        const visioncam::TaskInfo& ti = rs.tasks[i];
        ESP_LOGI(TAG, "  %-16s hwm=%6u B prio=%u core=%d state=%u",
                 ti.name, static_cast<unsigned>(ti.stack_hwm),
                 static_cast<unsigned>(ti.prio), static_cast<int>(ti.core),
                 static_cast<unsigned>(ti.state));
    }

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " VisionCam Pro — Phase 1 bring-up (smoke steps)");
    ESP_LOGI(TAG, "================================================");

    // --- step 10: reset cause (SPEC 2.4) -----------------------------------
    const esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "reset reason: %s", visioncam::resetReasonName(reason));
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
        visioncam::eventBusPublish(visioncam::EventType::CameraFailed,
                                   0, camErr);
    } else {
        ESP_LOGI(TAG, "CAMERA OK: pid=0x%04x, attempts=%u, warmup=%u, jpeg=%u B (%ux%u)",
                 cam.sensor_pid, cam.init_attempts, cam.warmup_discarded,
                 static_cast<unsigned>(cam.jpeg_bytes), cam.jpeg_width, cam.jpeg_height);
        visioncam::eventBusPublish(visioncam::EventType::CameraStarted);
        // NOTE: the health marker for the crash-loop counter is NOT set here
        // — it moved to the end of the boot sequence (after the verdict) so
        // a crash later in this boot still counts as abnormal. Evidence:
        // docs/evidence/phase1-core-boot-03.log (crash ~2.2 s, health marked
        // at ~0.8 s -> abnormal=0). See core_init.h for the health point.
    }

    // Event-bus round trip: the dispatcher runs at higher priority than
    // this task, so delivery is complete by the time the delay returns.
    vTaskDelay(pdMS_TO_TICKS(50));
    visioncam::EventBusStats ebs;
    visioncam::eventBusGetStats(ebs);
    const bool bus_ok = ebs.published >= 1 && ebs.delivered >= 1;
    ESP_LOGI(TAG, "event bus self-test: published=%u delivered=%u dropped=%u -> %s",
             static_cast<unsigned>(ebs.published),
             static_cast<unsigned>(ebs.delivered),
             static_cast<unsigned>(ebs.dropped), bus_ok ? "OK" : "FAIL");

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

    // --- step 14: camera + SD together, Wi-Fi off then Wi-Fi on ------------
    // A brownout during radio TX resets the chip, and the NEXT boot reports
    // reset reason BROWNOUT — so this log plus the following boot's banner
    // are the evidence pair for "no brownout". Wi-Fi here is only a local
    // SoftAP + scan (wifi_bringup.h): no credentials, no network joined.
    //
    // ORDER (docs/REFERENCE_ANALYSIS.md, "Camera driver ISR overflow warning
    // vs flash-cache-off windows"): the radio must be started with the camera
    // DOWN. esp_wifi_init() reads NVS with the flash cache disabled, and the
    // camera DMA ISR's overflow warning reads a flash-resident literal —
    // with both live the ISR faults (reproduced 3x, phase1-core-boot-03.log).
    // cameraShutdown() is NULL-safe (cam_deinit guards cam_obj), so this
    // sequence is safe even when the camera is already down.
    const char* step14 = "SKIP";
    if (camErr == ESP_OK && sd.self_test_ok) {
        // (a) baseline cycle. With no credentials the radio has never been
        // started ("Wi-Fi off"); with credentials the network manager owns
        // it (started before the camera at boot — the safe order) and this
        // cycle runs under whatever association it is doing.
        bool ok = togetherCycle("wifi off");

        if (visioncam::networkManagerRadioOwned()) {
            // The real network manager owns the radio: its association and
            // backoff traffic IS the TX load this test exists to survive,
            // and a SoftAP next to a live STA would be wrong. The radio was
            // started before the camera ISR existed (boot order above), so
            // the camera-down dance is not needed. Three seconds of real
            // network activity with camera + SD running concurrently.
            const visioncam::WifiStatus ws0 = visioncam::networkManagerStatus();
            ESP_LOGI(TAG, "step 14: network manager owns the radio (%s, "
                          "rssi %d dBm, connects=%u) — using real TX load",
                     visioncam::wifiStateName(ws0.state), ws0.rssi,
                     static_cast<unsigned>(ws0.connects));
            vTaskDelay(pdMS_TO_TICKS(3000));
            ok = togetherCycle("wifi on (network manager)") && ok;
            const visioncam::WifiStatus ws1 = visioncam::networkManagerStatus();
            step14 = ok ? "OK" : "FAIL";
            ESP_LOGI(TAG, "step 14 result: %s (network %s, rssi %d dBm, "
                          "connects=%u disconnects=%u attempts=%u backoff=%u ms)",
                     step14, visioncam::wifiStateName(ws1.state), ws1.rssi,
                     static_cast<unsigned>(ws1.connects),
                     static_cast<unsigned>(ws1.disconnects),
                     static_cast<unsigned>(ws1.attempts),
                     static_cast<unsigned>(ws1.backoff_ms));
        } else {
            // No credentials → manager left the driver alone → the Phase 0
            // SoftAP probe provides the TX load, in the hazard-safe order:
            // camera down, radio up, camera back, cycle, radio off. The
            // final wifiStop() runs with the camera UP (observed safe on
            // boot 4 of phase1-core-boot-03.log); the hazard is on the
            // wifiStart side only.
            visioncam::cameraShutdown();
            visioncam::WifiReport wr;
            const esp_err_t werr = visioncam::wifiStart(wr);
            if (werr != ESP_OK) {
                ESP_LOGE(TAG, "step 14: wifi start failed: %s", esp_err_to_name(werr));
                ok = false;
            } else {
                visioncam::CameraReport cam14;
                const esp_err_t rerr = visioncam::cameraBringup(cam14);
                if (rerr != ESP_OK) {
                    ESP_LOGE(TAG, "step 14: camera re-bringup after wifi start failed: %s",
                             esp_err_to_name(rerr));
                    ok = false;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(3000));  // sustain the beacon TX load
                    ok = togetherCycle("wifi on") && ok;
                    visioncam::WifiReport wr2;
                    const esp_err_t serr = visioncam::wifiStop(wr2);
                    if (serr != ESP_OK) {
                        ESP_LOGE(TAG, "step 14: wifi stop failed: %s", esp_err_to_name(serr));
                        ok = false;
                    }
                }
            }
            step14 = ok ? "OK" : "FAIL";
            ESP_LOGI(TAG, "step 14 result: %s (scan heard %u network(s))",
                     step14, static_cast<unsigned>(wr.scan_results));
        }
        logHeap("after wifi");
    } else {
        ESP_LOGW(TAG, "step 14 skipped — camera or SD not OK");
    }

    // --- step 11 (part 2): forced first-attempt failure -> PWDN recovery ----
    // Re-runs bring-up with PWDN held during attempt 1: the sensor is really
    // unreachable, so esp_camera_init must really fail, and the normal retry
    // (cameraShutdown + PWDN power-cycle) must recover on attempt 2.
    const char* forced = "SKIP";
    if (camErr == ESP_OK) {
        visioncam::cameraShutdown();
        visioncam::CameraReport cam2;
        const esp_err_t fErr = visioncam::cameraBringup(cam2, /*force=*/true);
        forced = (fErr == ESP_OK && cam2.init_attempts == 2) ? "OK" : "FAIL";
        ESP_LOGI(TAG, "forced-failure test: err=%s, attempts=%u -> %s",
                 esp_err_to_name(fErr), cam2.init_attempts, forced);
    } else {
        ESP_LOGW(TAG, "forced-failure test skipped — camera not OK");
    }

    // --- verdict -----------------------------------------------------------
    visioncam::EventBusStats ebs2;
    visioncam::eventBusGetStats(ebs2);
    ESP_LOGI(TAG, "------------------------------------------------");
    const visioncam::WifiStatus netSt = visioncam::networkManagerStatus();
    ESP_LOGI(TAG, " core: %s | camera: %s | sd: %s | psram: %s | net: %s | wifi(step14): %s | forced-retry: %s",
             coreErr == ESP_OK ? "OK" : "FAIL",
             camErr == ESP_OK ? "OK" : "FAIL",
             sdErr == ESP_OK ? (sd.self_test_ok ? "OK" : "SELFTEST-FAIL") : "FAIL",
             psramSize > 0 ? "OK" : "MISSING",
             visioncam::wifiStateName(netSt.state),
             step14,
             forced);
    ESP_LOGI(TAG, " events: published=%u delivered=%u dropped=%u | abnormal boots=%u",
             static_cast<unsigned>(ebs2.published),
             static_cast<unsigned>(ebs2.delivered),
             static_cast<unsigned>(ebs2.dropped),
             static_cast<unsigned>(visioncam::ringLogAbnormalBoots()));
    ESP_LOGI(TAG, "================================================");

    // Health point for the crash-loop counter (plan 02 task 1.1.1): reached
    // only when the whole boot sequence completed — anything that reset the
    // chip before this line increments abnormal_boots on the next boot.
    // When web/ lands (task 1.2) this stays at end-of-boot but the criterion
    // becomes "camera + HTTP serving"; Wi-Fi remains optional (SPEC 9).
    visioncam::ringLogMarkHealthy();
    ESP_LOGI(TAG, "health marked: abnormal boots now %u",
             static_cast<unsigned>(visioncam::ringLogAbnormalBoots()));

    // Keep the task alive so heap minima accumulate across the idle period.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
