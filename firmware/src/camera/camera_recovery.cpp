// camera_recovery.cpp — see camera_recovery.h.

#include "camera_recovery.h"

#include <atomic>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_manager.h"
#include "event_bus.h"
#include "events.h"
#include "ring_log.h"

namespace visioncam {
namespace {

const char* TAG = "vc_camrec";

// SPEC 9.1 rule 2: the two hard gates. Both are constants, not config:
// they are safety limits, not preferences.
constexpr int64_t  kAttemptIntervalMs = 2000;  // at most once every 2 s
constexpr unsigned kMaxAttempts       = 10;    // consecutive attempts
constexpr unsigned kCheapAttempts     = 5;     // rung 1 (sensor reset) first

// SPEC 9.1 rule 1: the yield the caller performs between NULL frames.
// Not used here — this module never loops; the value is published to the
// frame manager as kNullFrameYieldMs so the two cannot drift apart.
constexpr uint32_t kNullFrameYieldMs = 10;

std::atomic<CameraRecoveryState> s_state{CameraRecoveryState::Idle};
std::atomic<bool>                s_failed{false};

std::atomic<uint32_t> s_null_frames{0};
std::atomic<uint32_t> s_recovery_attempts{0};
std::atomic<uint32_t> s_sensor_resets{0};
std::atomic<uint32_t> s_re_inits{0};
std::atomic<uint32_t> s_rung_errors{0};
std::atomic<uint32_t> s_recovered{0};
std::atomic<uint32_t> s_failed_events{0};
std::atomic<int64_t>  s_last_attempt_ms{0};

int64_t nowMs() { return esp_timer_get_time() / 1000; }

}  // namespace

// The only spin protection the frame manager needs between NULL frames
// (SPEC 9.1 rule 1). Exposed so the caller cannot drift from this value.
uint32_t cameraRecoveryYieldMs() { return kNullFrameYieldMs; }

CameraRecoveryState cameraRecoveryState() { return s_state.load(); }
bool cameraRecoveryFailed() { return s_failed.load(); }

CameraRecoveryStats cameraRecoveryGetStats() {
    CameraRecoveryStats st;
    st.null_frames = s_null_frames.load();
    st.recovery_attempts = s_recovery_attempts.load();
    st.sensor_resets = s_sensor_resets.load();
    st.re_inits = s_re_inits.load();
    st.rung_errors = s_rung_errors.load();
    st.recovered = s_recovered.load();
    st.failed_events = s_failed_events.load();
    st.last_attempt_ms = s_last_attempt_ms.load();
    return st;
}

NullFrameVerdict cameraRecoveryNoteNullFrame() {
    s_null_frames.fetch_add(1);

    // Terminal: the ladder already gave up. Caller must have stopped its
    // consumers (SPEC 9.1 rule 4) — if it did not, it still never spins.
    if (s_failed.load()) return NullFrameVerdict::Failed;

    if (s_state.load() == CameraRecoveryState::Idle) {
        s_state.store(CameraRecoveryState::Recovering);
    }

    // Gate 1 (2 s interval): nothing to run yet. The caller's ~10 ms yield
    // is the ONLY spin protection here — there is no tight retry.
    const int64_t now = nowMs();
    const int64_t last = s_last_attempt_ms.load();
    if (last != 0 && (now - last) < kAttemptIntervalMs) {
        return NullFrameVerdict::Yield;
    }

    // Gate 2 (attempt cap).
    const unsigned attempts = s_recovery_attempts.load();
    if (attempts >= kMaxAttempts) {
        s_failed.store(true);
        s_state.store(CameraRecoveryState::Failed);
        s_failed_events.fetch_add(1);
        eventBusPublish(EventType::CameraFailed, attempts);
        VC_RING_ERROR("camera", "NULL-frame recovery exhausted after %u "
                                "attempt(s) — consumers must stop",
                      attempts);
        ESP_LOGE(TAG, "recovery exhausted: %u attempts, %u NULL frames — "
                      "CameraFailed, consumers stopped",
                 attempts, s_null_frames.load());
        return NullFrameVerdict::Failed;
    }

    // Run the due rung IN THE CALLER'S TASK (single-caller contract).
    s_recovery_attempts.store(attempts + 1);
    s_last_attempt_ms.store(now);

    NullFrameVerdict verdict = NullFrameVerdict::Recovered;
    if (attempts < kCheapAttempts) {
        // Rung 1: sensor software reset + re-apply settings.
        s_sensor_resets.fetch_add(1);
        const esp_err_t err = cameraManagerSensorReset();
        if (err != ESP_OK) {
            s_rung_errors.fetch_add(1);
            verdict = NullFrameVerdict::Yield;
        }
        ESP_LOGW(TAG, "rung 1 (sensor reset) attempt %u/%u: %s",
                 attempts + 1, static_cast<unsigned>(kMaxAttempts),
                 esp_err_to_name(err));
    } else {
        // Rung 2: hardware power-cycle + full SPEC 4.1.1 re-init. The heavy
        // rung — it churns PSRAM buffers, so it comes second (header).
        s_re_inits.fetch_add(1);
        visioncam::CameraReport report;
        const esp_err_t err = cameraManagerReinit(report);
        if (err != ESP_OK) {
            s_rung_errors.fetch_add(1);
            verdict = NullFrameVerdict::Yield;
        }
        ESP_LOGW(TAG, "rung 2 (power-cycle + re-init) attempt %u/%u: %s",
                 attempts + 1, static_cast<unsigned>(kMaxAttempts),
                 esp_err_to_name(err));
    }
    return verdict;
}

void cameraRecoveryNoteGoodFrame() {
    const CameraRecoveryState st = s_state.load();
    if (st == CameraRecoveryState::Recovering ||
        st == CameraRecoveryState::Recovered) {
        // SPEC 9.1 rule 3: a valid frame resets the consecutive counters.
        s_recovery_attempts.store(0);
        s_last_attempt_ms.store(0);
        s_recovered.fetch_add(1);
        eventBusPublish(EventType::CameraRecovered,
                        static_cast<uint16_t>(s_recovery_attempts.load()));
        VC_RING_INFO("camera", "valid frame — recovery counters reset");
        ESP_LOGI(TAG, "valid frame after recovery — counters reset");
    }
    s_state.store(CameraRecoveryState::Idle);
}

void cameraRecoveryReset() {
    s_failed.store(false);
    s_state.store(CameraRecoveryState::Idle);
    s_recovery_attempts.store(0);
    s_sensor_resets.store(0);
    s_re_inits.store(0);
    s_rung_errors.store(0);
    s_recovered.store(0);
    s_failed_events.store(0);
    s_last_attempt_ms.store(0);
    ESP_LOGI(TAG, "recovery ladder reset");
}

}  // namespace visioncam