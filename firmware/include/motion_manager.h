// motion_manager.h — motion detection engine (Phase 4 task 4.1; SPEC §4.3).
//
// Lightweight motion detection independent of AI. Decodes each camera frame
// to a small grayscale bitmap using esp_jpeg_decode, compares with the
// previous sample using per-block Sum of Absolute Differences (SAD), and
// publishes MotionStarted/MotionEnded events on the typed event bus.
//
// Design:
//
//   capture task (prio 6)  --frame lease-->  motion task (prio 4)
//                                                    |
//                                            decode JPEG → 96x96 grayscale
//                                                    |
//                                            SAD compare with prev sample
//                                                    |
//                                            publish MotionStarted/MotionEnded
//
// The motion task acquires frame leases from the frame manager (AGENTS rule 9:
// only the frame manager touches camera buffers). It copies the JPEG into its
// own bounded buffer, releases the lease immediately (never holds across
// blocking I/O — SPEC 4.1.1 rule 9), then decodes and analyzes.
//
// Two sample rates (R1 pattern, both configurable):
//   - Idle:     higher rate (~5 checks/s default)
//   - Recording: lower rate (1 check/2 s default, also post-motion hold time)
//
// Detection aborted above SXGA (decoder limit).
//
// Bounds (AGENTS rules 12-13):
//   kMotionJpegMaxBytes   — fixed 64 KB JPEG staging buffer
//   kMotionGrayPixels     — fixed 96x96 = 9216 B grayscale buffer
//   kMotionQueueDepth     — bounded event queue via event bus (drop-oldest)

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "frame_manager.h"

namespace visioncam {

// ---- Motion configuration (SPEC §4.3) ------------------------------------

struct MotionConfig {
    bool enabled = false;                 // motion detection off by default
    uint32_t sample_rate_idle_hz = 5;     // checks/s while idle (R1: ~5)
    uint32_t sample_rate_rec_hz = 0;      // 0 = 1 check/2 s (R1 default)
    uint32_t threshold = 8;               // per-pixel intensity threshold (0-255)
    uint32_t min_changed_ratio = 15;      // percent of blocks that must change
    uint32_t cooldown_ms = 2000;          // suppress repeats within this window
    uint32_t min_frames = 5;              // min consecutive changed frames to confirm
    uint32_t settle_frames = 3;           // warm-up frames to ignore
    bool record_on_motion = true;         // trigger event recording on motion
    bool snapshot_on_motion = true;       // trigger snapshot on motion
    bool mqtt_notify = true;              // publish MQTT event on motion
};

// ---- Motion status --------------------------------------------------------

struct MotionStatus {
    bool enabled = false;
    bool active = false;                  // motion currently detected
    uint32_t events_total = 0;            // motion-start events published
    uint32_t events_last_ts_ms = 0;       // timestamp of last event
    uint32_t checks_total = 0;            // total analysis cycles
    uint32_t checks_skipped = 0;          // skipped (busy / not idle)
    uint32_t changed_ratio_last = 0;      // last changed-pixel ratio (0-100)
    uint32_t light_level_last = 0;        // last ambient light level (0-100)
    uint32_t decode_time_ms_last = 0;     // last JPEG decode time (ms)
    uint32_t analyze_time_ms_last = 0;    // last analysis time (ms)
};

// ---- Public API ----------------------------------------------------------

// Initialise motion engine. Call after frame manager start and storage
// manager init. Subscribes to frame manager and spawns the motion task.
esp_err_t motionManagerInit(const MotionConfig* config);

// Shut down motion task and unsubscribe from frame manager.
esp_err_t motionManagerShutdown();

// Get config (copy).
esp_err_t motionManagerGetConfig(MotionConfig* out);

// Set config (parse → validate → persist → apply → return sanitised,
// SPEC §7 process). Partial updates overlay on current config.
esp_err_t motionManagerSetConfig(const MotionConfig* candidate,
                                 MotionConfig* effective);

// Get current motion status.
esp_err_t motionManagerStatus(MotionStatus* out);

// Enable/disable motion detection at runtime.
esp_err_t motionManagerSetEnabled(bool enabled);

// ---- Internal: called by frame manager lease path ------------------------

// Process one acquired frame lease. Decodes JPEG, analyzes motion,
// publishes events as needed. Called from the motion task.
esp_err_t motionProcessFrame(const FrameLease& lease);

}  // namespace visioncam
