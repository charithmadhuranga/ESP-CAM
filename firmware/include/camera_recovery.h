// camera_recovery.h — camera NULL-frame recovery ladder (plan 02 task
// 1.1.4; SPEC section 9.1, "from R1" — behaviour adopted, implementation
// fresh in our own words).
//
// SPEC 9.1, verbatim rules:
//   1. never retry in a tight loop — yield ~10 ms per NULL (task watchdog)
//   2. recovery at most once every 2 s, at most 10 consecutive times,
//      starting with a sensor reset, then PWDN power-cycle + re-init
//   3. counters reset when a VALID frame arrives
//   4. after the limit: raise CameraFailed, stop consumers cleanly,
//      expose state (UI/MQTT later), do not spin
//   5. count dropped and invalid frames in metrics
//
// Single-caller contract: the ladder is driven by the frame manager's
// capture task (task 1.1.5) — the only consumer of camera frames — from
// noteNullFrame()/noteGoodFrame(). No task, no timer, no lock is needed:
// one caller, one thread, and the gates below are the entire state
// machine. The stats are atomic so /metrics and /status can read them
// while it runs.
//
// Rung split (our choice, documented): the SPEC names two rungs and our
// hardware sequence prefers the cheap one first, because a sensor reset
// does not churn the PSRAM frame buffers. Rung 1 = sensor software reset
// for attempts 1..kCheapAttempts; rung 2 = PWDN power-cycle + full
// SPEC 4.1.1 re-init for the rest. After the 10th failed attempt the
// ladder is terminal (Failed) until a good frame or a manual reset.

#pragma once

#include <cstdint>

#include "esp_err.h"

namespace visioncam {

// What the caller (frame manager) must do next after a NULL frame.
enum class NullFrameVerdict : uint8_t {
    Yield,      // no recovery ran (2 s gate, or a rung errored):
                // yield ~10 ms and poll again — never spin
    Recovered,  // a recovery rung ran successfully: keep polling frames
    Failed,     // ladder exhausted: CameraFailed raised — stop consumers
};

// Ladder state for /status and the UI. Failed stays until reset.
enum class CameraRecoveryState : uint8_t {
    Idle = 0,   // no consecutive NULL frames
    Recovering, // NULL frames seen, rungs running
    Recovered,  // a valid frame arrived after recovery attempts (clears
                // to Idle)
    Failed,     // 10 attempts exhausted; consumers must stop
};

CameraRecoveryState cameraRecoveryState();
bool cameraRecoveryFailed();

// The ~10 ms yield the caller must perform between NULL frames
// (SPEC 9.1 rule 1). Exposed so the frame manager cannot drift from it.
uint32_t cameraRecoveryYieldMs();

// Call from the frame manager when esp_camera_fb_get() returned NULL.
// Applies the SPEC 9.1 gates internally and runs the due rung. Runs the
// rung in the CALLER's task (a heavy rung can block ~0.5 s — the price of
// recovering the camera, and it happens at most 5 times per failure).
NullFrameVerdict cameraRecoveryNoteNullFrame();

// Call from the frame manager on every VALID frame: resets the SPEC 9.1
// rule-3 counters and leaves Failed only via cameraRecoveryReset().
void cameraRecoveryNoteGoodFrame();

// Clears the terminal Failed state (restart endpoint / manual retry).
void cameraRecoveryReset();

struct CameraRecoveryStats {
    uint32_t null_frames = 0;        // every NULL esp_camera_fb_get()
    uint32_t recovery_attempts = 0;  // rungs actually executed
    uint32_t sensor_resets = 0;      // rung 1 executions
    uint32_t re_inits = 0;           // rung 2 executions
    uint32_t rung_errors = 0;        // rung executions that returned an error
    uint32_t recovered = 0;          // times a good frame reset the counters
    uint32_t failed_events = 0;      // CameraFailed raised (>=1 = terminal)
    int64_t  last_attempt_ms = 0;    // esp_timer ms of the last rung
};

CameraRecoveryStats cameraRecoveryGetStats();

}  // namespace visioncam