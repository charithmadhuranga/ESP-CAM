// Phase 0 smoke firmware — plan 01-phase-0-baseline.md, task 0.3.
//
//   step 10  reset reason, PSRAM, heap reporting
//   step 11  camera bring-up        (visioncam::cameraManagerInit)
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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_profile.h"
#include "camera_manager.h"
#include "camera_recovery.h"
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
// together" probe of plan 0.3 step 14. The grab is the temporary smoke
// probe of camera_manager.h (replaced by a frame/ lease in task 1.1.5)
// and file access stays in sd_bringup (AGENTS rule 10); this helper only
// orchestrates the two.
static bool togetherCycle(const char* phase) {
    size_t bytes = 0;
    const bool cam_ok = visioncam::cameraManagerGrabProbe(&bytes);
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
    esp_log_level_set("vc_camrec", ESP_LOG_INFO);
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
    const esp_err_t camErr = visioncam::cameraManagerInit(cam);
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
            visioncam::cameraManagerShutdown();
            visioncam::WifiReport wr;
            const esp_err_t werr = visioncam::wifiStart(wr);
            if (werr != ESP_OK) {
                ESP_LOGE(TAG, "step 14: wifi start failed: %s", esp_err_to_name(werr));
                ok = false;
            } else {
                visioncam::CameraReport cam14;
                const esp_err_t rerr = visioncam::cameraManagerInit(cam14);
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
    // (cameraManagerShutdown + PWDN power-cycle) must recover on attempt 2.
    const char* forced = "SKIP";
    if (camErr == ESP_OK) {
        visioncam::cameraManagerShutdown();
        visioncam::CameraReport cam2;
        const esp_err_t fErr = visioncam::cameraManagerInit(
            cam2, visioncam::CameraInitTestMode::ForceFirstFailure);
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

    // --- step 15: SPEC 9.1 NULL-frame recovery ladder ---------------------
    // The ladder is normally driven by frame/'s capture task (task 1.1.5),
    // which does not exist yet, so this drives it directly — honestly: the
    // camera is held DOWN (shutdown + sticky XCLK sabotage) so every
    // esp_camera_fb_get the ladder's rungs rely on really would return
    // NULL, and every re-init really fails. The result must be: 5 cheap
    // rungs (sensor reset) + 5 heavy rungs (power-cycle + re-init), each
    // gated to one per 2 s, then exactly one terminal CameraFailed — no
    // spin, no reset before a valid frame.
    const char* ladder = "SKIP";
    if (camErr == ESP_OK) {
        visioncam::cameraManagerShutdown();
        visioncam::cameraManagerSetTestSabotage(true);   // hold the camera dead
        visioncam::cameraRecoveryReset();

        unsigned yields = 0, recovered_rungs = 0, failed_verdicts = 0;
        const int64_t t0 = esp_timer_get_time() / 1000;
        int64_t tFailed = 0;
        for (;;) {
            const visioncam::NullFrameVerdict v =
                visioncam::cameraRecoveryNoteNullFrame();
            if (v == visioncam::NullFrameVerdict::Failed) {
                ++failed_verdicts;
                tFailed = esp_timer_get_time() / 1000;
            } else if (v == visioncam::NullFrameVerdict::Recovered) {
                ++recovered_rungs;
            } else {
                ++yields;
            }
            if (failed_verdicts > 0) break;
            if ((esp_timer_get_time() / 1000) - t0 > 30000) break;  // safety cap
            // SPEC 9.1 rule 1: the yield itself comes from the ladder API so
            // the frame manager cannot drift from it.
            vTaskDelay(pdMS_TO_TICKS(visioncam::cameraRecoveryYieldMs()));
        }
        const visioncam::CameraRecoveryStats rs = visioncam::cameraRecoveryGetStats();

        // Bring the camera back and prove the rule-3 reset path: a valid
        // frame after recovery returns the ladder to Idle.
        visioncam::cameraManagerSetTestSabotage(false);
        const bool ladder_terminal =
            visioncam::cameraRecoveryState() == visioncam::CameraRecoveryState::Failed;
        visioncam::cameraRecoveryReset();
        visioncam::CameraReport cam15;
        const esp_err_t backErr = visioncam::cameraManagerInit(cam15);
        size_t frame_bytes = 0;
        const bool frame_ok = (backErr == ESP_OK) &&
                              visioncam::cameraManagerGrabProbe(&frame_bytes);
        if (frame_ok) visioncam::cameraRecoveryNoteGoodFrame();
        const bool ladder_reset =
            visioncam::cameraRecoveryState() == visioncam::CameraRecoveryState::Idle;

        // Verdict, with the SPEC 9.1 numbers that make it honest: attempts
        // must be exactly 10, all ten gated (elapsed >= 18 s: 9 x 2 s gates),
        // exactly one CameraFailed verdict, and the reset path must work.
        const bool gates_ok = rs.recovery_attempts == 10 &&
                              (tFailed - t0) >= 18000 &&
                              rs.sensor_resets == 5 && rs.re_inits == 5 &&
                              rs.rung_errors == 10 && failed_verdicts == 1;
        ladder = (ladder_terminal && gates_ok && frame_ok && ladder_reset)
                     ? "OK" : "FAIL";
        ESP_LOGI(TAG, "step 15 result: %s (attempts=%u rung1=%u rung2=%u "
                      "errors=%u yields=%u elapsed=%lld ms; failed_verdicts=%u "
                      "terminal=%d; back-up frame=%u B, ladder reset=%d)",
                 ladder, static_cast<unsigned>(rs.recovery_attempts),
                 static_cast<unsigned>(rs.sensor_resets),
                 static_cast<unsigned>(rs.re_inits),
                 static_cast<unsigned>(rs.rung_errors), yields,
                 static_cast<long long>(tFailed - t0), failed_verdicts,
                 ladder_terminal ? 1 : 0,
                 static_cast<unsigned>(frame_bytes), ladder_reset ? 1 : 0);
        if (ladder[0] != 'O' || ladder[1] != 'K') {
            ESP_LOGE(TAG, "step 15 FAILED — SPEC 9.1 ladder evidence: stats "
                          "nulls=%u attempts=%u recovered=%u failed_events=%u",
                     static_cast<unsigned>(rs.null_frames),
                     static_cast<unsigned>(rs.recovery_attempts),
                     static_cast<unsigned>(rs.recovered),
                     static_cast<unsigned>(rs.failed_events));
        }
    } else {
        ESP_LOGW(TAG, "step 15 skipped — camera not OK");
    }

    // Keep the task alive so heap minima accumulate across the idle period.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
