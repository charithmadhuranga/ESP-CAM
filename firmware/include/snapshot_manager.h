// snapshot_manager.h — Snapshot capture + save (plan 02 task 2.3; SPEC
// sections 4.5, 6.5).
//
// The snapshot manager is a consumer of the frame manager (AGENTS rule 9:
// only frame/ touches camera buffers). It acquires a frame lease, copies
// the JPEG into its own bounded buffer, releases the lease immediately,
// then writes to SD via the storage manager (AGENTS rule 10).
//
// Public API uses opaque media IDs (med-…), never filesystem paths
// (AGENTS rule 15).

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

#include "frame_manager.h"
#include "storage_manager.h"

namespace visioncam {

// Snapshot configuration.
struct SnapshotConfig {
    framesize_t framesize = FRAMESIZE_VGA;  // capture resolution
    uint8_t     jpeg_quality = 12;          // 1..63, lower = better
    uint32_t    cooldown_ms = 2000;         // suppress dupes within this
    bool        enabled = true;
};

// Snapshot manager. One instance; created by snapshotManagerInit().
class SnapshotManager {
public:
    explicit SnapshotManager() = default;

    // Initialise: subscribe to frame manager as a consumer. Must be called
    // after frameManagerInit() and storageManagerInit().
    esp_err_t init();

    // Capture a snapshot manually. Returns the media_id in out_id (>=24 B).
    esp_err_t captureManual(char* out_id, size_t out_cap);

    // Capture a motion-triggered snapshot. Returns ESP_OK even if
    // suppressed by cooldown (out_id will be empty).
    esp_err_t captureMotion(char* out_id, size_t out_cap);

    // Capture a detection-triggered snapshot.
    esp_err_t captureDetection(char* out_id, size_t out_cap);

    // True if the manager is initialised and ready.
    bool available() const { return running_; }

    // Get current config.
    SnapshotConfig config() const { return config_; }

    // Set config (applies live).
    void setConfig(const SnapshotConfig& cfg) { config_ = cfg; }

private:
    esp_err_t doCapture(SnapshotSource source, char* out_id, size_t out_cap);

    bool running_ = false;
    FrameSubscriberId sub_id_ = 0;
    SnapshotConfig config_;
    uint32_t last_capture_ts_us_[4] = {0, 0, 0, 0};  // per-source cooldown
};

// Global accessor (thin wrapper, like storage_manager).
esp_err_t snapshotManagerInit();
bool snapshotManagerAvailable();
esp_err_t snapshotCaptureManual(char* out_id, size_t out_cap);
esp_err_t snapshotCaptureMotion(char* out_id, size_t out_cap);
esp_err_t snapshotCaptureDetection(char* out_id, size_t out_cap);

}  // namespace visioncam
