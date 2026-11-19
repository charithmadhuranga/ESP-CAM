// snapshot_manager.cpp — Snapshot capture + save (plan 02 task 2.3; SPEC
// sections 4.5, 6.5).
//
// Acquires a frame lease from the frame manager, copies the JPEG into a
// bounded buffer, releases the lease immediately (never holds across SD
// I/O — SPEC 4.1.1 rule 9), then writes via storage manager (AGENTS rule 10).

#include "snapshot_manager.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdlib>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ring_log.h"
#include "wall_clock.h"

namespace visioncam {
namespace {

const char* TAG = "vc_snapshot";

// Maximum JPEG size we'll buffer (VGA at quality 12 ≈ 100-200 KB;
// PSRAM can hold this). Bounded — never sized from user input.
constexpr size_t kMaxJpegBytes = 512 * 1024;

// Snapshot directory: /sdcard/snapshots/YYYY/MM/DD/{category}/
constexpr char kSnapshotRoot[] = "/sdcard/snapshots";

}  // namespace

// ========================================================================
// SnapshotManager implementation
// ========================================================================

esp_err_t SnapshotManager::init() {
    if (running_) return ESP_ERR_INVALID_STATE;
    if (!visioncam::storageManagerAvailable()) {
        ESP_LOGW(TAG, "init: storage not available");
        return ESP_ERR_INVALID_STATE;
    }

    // Note: we do NOT subscribe to the frame manager here. A long-lived
    // subscriber's queue can go stale after a camera shutdown/restart
    // cycle (step 15), causing frameAcquire() to time out. Instead,
    // doCapture() registers a fresh subscriber for each capture.
    running_ = true;
    ESP_LOGI(TAG, "init: ready (fresh subscriber per capture)");
    return ESP_OK;
}

esp_err_t SnapshotManager::doCapture(SnapshotSource source,
                                     char* out_id, size_t out_cap) {
    if (!running_) return ESP_ERR_INVALID_STATE;
    if (out_id == nullptr || out_cap < 24) return ESP_ERR_INVALID_ARG;
    out_id[0] = '\0';

    // Never write a 1970/01/01 path. wallClockSeconds() deliberately falls
    // back to uptime when SNTP has not set the clock yet, and an uptime
    // value read as an epoch IS 1970-01-01 — which produced a second
    // snapshot tree on this board (/sdcard/snapshots/1970/01/01/manual/)
    // holding empty, unfetchable files. Refuse instead of writing a path
    // retention can never date correctly; the caller sees
    // TimeUnsynchronised and the capture succeeds on a later attempt.
    if (wallClockSeconds() < kWallClockEraThreshold) {
        ESP_LOGW(TAG,
                 "doCapture: wall clock unset (uptime %u s) — refusing an "
                 "epoch-dated snapshot, retry after time sync",
                 static_cast<unsigned>(wallClockSeconds()));
        return ESP_ERR_INVALID_STATE;
    }

    // Cooldown check per source.
    const uint32_t now_us = esp_timer_get_time();
    const uint32_t idx = static_cast<uint32_t>(source);
    if (config_.cooldown_ms > 0 &&
        last_capture_ts_us_[idx] != 0 &&
        (now_us - last_capture_ts_us_[idx]) <
            static_cast<uint64_t>(config_.cooldown_ms) * 1000ULL) {
        ESP_LOGD(TAG, "suppressed by cooldown (source=%u)", idx);
        return ESP_OK; // suppressed, not an error
    }

    // Acquire a frame lease. Don't hold it across I/O. Register a fresh
    // subscriber for each capture — a long-lived subscriber's queue can
    // become stale after a camera shutdown/restart cycle (step 15), and
    // frameAcquire() then times out even though fresh subscribers work.
    FrameLease lease{};
    FrameSubscriberId cap_sub = 0;
    const esp_err_t serr = frameManagerSubscribe(&cap_sub, "snapshot");
    if (serr != ESP_OK) {
        ESP_LOGE(TAG, "doCapture: subscribe failed: %s", esp_err_to_name(serr));
        return serr;
    }
    ESP_LOGD(TAG, "doCapture: subscribed as sub=%u", static_cast<unsigned>(cap_sub));
    // Give the capture task a tick to deliver a frame to the new queue.
    vTaskDelay(pdMS_TO_TICKS(10));
    bool acquired = frameAcquire(cap_sub, lease, 30000 /* timeout_ms */);
    if (!acquired) {
        ESP_LOGE(TAG, "doCapture: frame acquire failed (sub=%u)",
                 static_cast<unsigned>(cap_sub));
        frameManagerUnsubscribe(cap_sub);
        return ESP_FAIL;
    }
    if (!lease.valid || lease.len == 0 || lease.data == nullptr) {
        frameRelease(lease);
        frameManagerUnsubscribe(cap_sub);
        ESP_LOGW(TAG, "doCapture: invalid frame (valid=%u len=%u)",
                 lease.valid ? 1 : 0, static_cast<unsigned>(lease.len));
        return ESP_FAIL;
    }
    if (lease.len > kMaxJpegBytes) {
        frameRelease(lease);
        frameManagerUnsubscribe(cap_sub);
        ESP_LOGE(TAG, "doCapture: frame too large (%u bytes)",
                 static_cast<unsigned>(lease.len));
        return ESP_ERR_NO_MEM;
    }

    // Copy into our own bounded buffer, then release the lease immediately.
    // Allocate from PSRAM-capable heap — a 512 KB stack buffer would overflow
    // the HTTP server's 4 KB task stack.
    uint8_t* jpeg_buf = static_cast<uint8_t*>(heap_caps_malloc(kMaxJpegBytes, MALLOC_CAP_DEFAULT));
    if (jpeg_buf == nullptr) {
        frameRelease(lease);
        frameManagerUnsubscribe(cap_sub);   // release the slot on every exit path
        ESP_LOGE(TAG, "doCapture: heap alloc failed for %u bytes",
                 static_cast<unsigned>(kMaxJpegBytes));
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t cerr = frameLeaseCopyTo(lease, jpeg_buf, kMaxJpegBytes);
    // Save the length BEFORE releasing: frameRelease() zeroes the lease
    // (a second release must be a no-op), so lease.len read afterwards is 0
    // and the index would record size 0 for every snapshot.
    const uint32_t jpeg_len = lease.len;
    frameRelease(lease);
    frameManagerUnsubscribe(cap_sub);
    if (cerr != ESP_OK) {
        free(jpeg_buf);
        ESP_LOGE(TAG, "doCapture: frame copy failed: %s", esp_err_to_name(cerr));
        return cerr;
    }

    // Build timestamped path: /sdcard/snapshots/YYYY/MM/DD/{category}/med-XXXXXXXX.jpg
    // Wall clock, not uptime — an uptime-based date yields 1970/01/01 and
    // every capture then collides in one directory (see wall_clock.h).
    // time_t is 64-bit on ESP-IDF: never gmtime_r() a 4-byte uint32_t
    // (the high half would be stack garbage → year 85818208).
    const time_t now_s = static_cast<time_t>(wallClockSeconds());
    std::tm tm{};
    gmtime_r(&now_s, &tm);

    char date_dir[64] = {};
    std::snprintf(date_dir, sizeof(date_dir), "%04d/%02d/%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    const char* cat_name = "manual";
    MediaCategory cat = MediaCategory::Manual;
    switch (source) {
        case SnapshotSource::Motion:
            cat_name = "motion";
            cat = MediaCategory::Motion;
            break;
        case SnapshotSource::Detection:
            cat_name = "detection";
            cat = MediaCategory::Detection;
            break;
        case SnapshotSource::Timelapse:
            cat_name = "timelapse";
            cat = MediaCategory::Timelapse;
            break;
        default:
            break;
    }

    char dir_path[128] = {};
    std::snprintf(dir_path, sizeof(dir_path), "%s/%s/%s",
                  kSnapshotRoot, date_dir, cat_name);

    // Allocate media ID first (for the filename).
    char media_id[24] = {};
    const esp_err_t iderr = storageAllocateMediaId(MediaType::Snapshot,
                                                   media_id, sizeof(media_id));
    if (iderr != ESP_OK) {
        ESP_LOGE(TAG, "doCapture: media ID allocation failed: %s",
                 esp_err_to_name(iderr));
        return iderr;
    }

    char file_path[200] = {};
    std::snprintf(file_path, sizeof(file_path), "%s/%s.jpg", dir_path,
                  media_id);

    // Write the JPEG via the storage manager's backend (temp + rename,
    // parent dirs created automatically). jpeg_buf was allocated above.
    const esp_err_t werr =
        storageWriteFile(file_path, jpeg_buf, jpeg_len);
    free(jpeg_buf);
    if (werr != ESP_OK) {
        ESP_LOGE(TAG, "doCapture: storage write failed: %s",
                 esp_err_to_name(werr));
        return werr;
    }

    // Record in media index (pass the pre-allocated media_id to avoid
    // double allocation).
    MediaRecord rec{};
    const esp_err_t rerr = storageRecordSnapshot(media_id, cat, nullptr,
                                                 file_path, jpeg_len, &rec);
    if (rerr != ESP_OK) {
        ESP_LOGE(TAG, "doCapture: storageRecordSnapshot failed: %s",
                 esp_err_to_name(rerr));
        return rerr;
    }

    last_capture_ts_us_[idx] = now_us;
    std::strncpy(out_id, media_id, out_cap - 1);
    out_id[out_cap - 1] = '\0';

    ESP_LOGI(TAG, "doCapture: saved %s (%u bytes, source=%u)", media_id,
             static_cast<unsigned>(jpeg_len), idx);
    return ESP_OK;
}

esp_err_t SnapshotManager::captureManual(char* out_id, size_t out_cap) {
    if (!config_.enabled) return ESP_ERR_INVALID_STATE;
    return doCapture(SnapshotSource::Manual, out_id, out_cap);
}

esp_err_t SnapshotManager::captureMotion(char* out_id, size_t out_cap) {
    return doCapture(SnapshotSource::Motion, out_id, out_cap);
}

esp_err_t SnapshotManager::captureDetection(char* out_id, size_t out_cap) {
    return doCapture(SnapshotSource::Detection, out_id, out_cap);
}

// ========================================================================
// Public API wrappers
// ========================================================================

namespace {

SnapshotManager* g_snap = nullptr;

}  // namespace

esp_err_t snapshotManagerInit() {
    if (g_snap != nullptr) return ESP_ERR_INVALID_STATE;
    g_snap = new SnapshotManager();
    if (g_snap == nullptr) return ESP_ERR_NO_MEM;
    return g_snap->init();
}

bool snapshotManagerAvailable() {
    return g_snap != nullptr && g_snap->available();
}

esp_err_t snapshotCaptureManual(char* out_id, size_t out_cap) {
    if (g_snap == nullptr) return ESP_ERR_INVALID_STATE;
    return g_snap->captureManual(out_id, out_cap);
}

esp_err_t snapshotCaptureMotion(char* out_id, size_t out_cap) {
    if (g_snap == nullptr) return ESP_ERR_INVALID_STATE;
    return g_snap->captureMotion(out_id, out_cap);
}

esp_err_t snapshotCaptureDetection(char* out_id, size_t out_cap) {
    if (g_snap == nullptr) return ESP_ERR_INVALID_STATE;
    return g_snap->captureDetection(out_id, out_cap);
}

}  // namespace visioncam
