// timelapse_manager.h — timelapse scheduler + project manager (Phase 5 task
// 5.1-5.4; SPEC §4.7).
//
// Timelapse uses scheduled JPEG captures as source frames. Two modes per
// project:
//   - Direct-to-AVI (default): each captured frame is appended to a per-project
//     AVI segment; segments close at duration/frame cap. No generation job.
//   - Frames + generate: keep individual JPEG source frames; build an AVI
//     later as a background job with progress/cancellation.
//
// Project model:
//   project.json per project under /timelapse/{project_id}/ with
//   schema_version: name, state, interval, start/end schedule, resolution,
//   quality, playback FPS, retention, counters, timestamps.
//
// All public APIs take opaque project IDs (tl-…), never filesystem paths
// (AGENTS rule 15, SPEC 6.0).
//
// Concurrency: timelapse shares the capture loop and storage writer —
// contention is the main risk. Timelapse runs at prio 3 (below motion at 4,
// below writer at 5, below capture at 6) so streaming/recording/motion
// always win. Mutual exclusion with continuous recording is enforced if
// Phase 5 measurements show insufficient headroom (clear UI message).
//
// Time synchronisation: timelapse refuses to start until wall clock is
// synchronised (wallClockSeconds() >= kWallClockEraThreshold).

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

#include "esp_err.h"

namespace visioncam {

// ---- Project state --------------------------------------------------------

enum class TimelapseState : uint8_t {
    Idle,        // not configured / stopped
    Capturing,   // actively capturing frames
    Ready,       // output available (frames + optional AVI)
    Error,       // error state (see error_code)
};

// ---- Project config -------------------------------------------------------

struct TimelapseConfig {
    char name[64] = {};
    uint32_t interval_s = 5;           // capture interval in seconds
    uint32_t duration_min = 0;         // 0 = unlimited / until stopped
    uint32_t start_time = 0;           // epoch start (0 = now)
    uint32_t end_time = 0;             // epoch end (0 = unlimited)
    uint32_t width = 640;              // capture resolution
    uint32_t height = 480;
    uint32_t quality = 12;             // JPEG quality (0-63)
    uint32_t playback_fps = 10;        // output playback FPS
    uint32_t retention_days = 30;      // retention for output
    bool mode_direct_to_avi = true;    // true = direct-to-AVI, false = frames+generate
};

// ---- Project info (listed in API) -----------------------------------------

struct TimelapseProject {
    char project_id[24];        // "tl-XXXXXXXX"
    char name[64];
    TimelapseState state = TimelapseState::Idle;
    uint32_t interval_s = 0;
    uint32_t duration_min = 0;  // total capture duration in minutes
    uint32_t progress_pct = 0;  // 0-100 capture progress
    uint32_t frame_count = 0;
    uint32_t size_bytes = 0;
    uint32_t start_time = 0;
    uint32_t end_time = 0;
    char error_code[32] = {};   // "unsynchronized" | "storage_full" | ""
};

// ---- Public API ----------------------------------------------------------

// Initialise timelapse manager. Call after frame manager start, storage
// manager init, and motion manager init (for coexistence checks).
esp_err_t timelapseManagerInit();

// Shut down timelapse task.
esp_err_t timelapseManagerShutdown();

// Get config (copy).
esp_err_t timelapseManagerGetConfig(TimelapseConfig* out);

// Set config (parse → validate → persist → apply → return sanitised,
// SPEC 7 process). Partial updates overlay on current config.
esp_err_t timelapseManagerSetConfig(const TimelapseConfig* candidate,
                                    TimelapseConfig* effective);

// Create a new timelapse project. Returns project_id.
esp_err_t timelapseManagerCreateProject(const TimelapseConfig* config,
                                        char* out_id, size_t out_cap);

// Get project info by ID.
esp_err_t timelapseManagerGetProject(const char* project_id,
                                     TimelapseProject* out);

// List projects (paginated).
esp_err_t timelapseManagerListProjects(uint32_t offset, uint32_t limit,
                                       TimelapseProject* out,
                                       size_t out_cap,
                                       size_t* out_count,
                                       bool* out_has_more);

// Start a project (begin capturing).
esp_err_t timelapseManagerStartProject(const char* project_id);

// Stop a project.
esp_err_t timelapseManagerStopProject(const char* project_id);

// Generate output AVI from source frames (frames+generate mode only).
// Returns job_id.
esp_err_t timelapseManagerGenerateProject(const char* project_id,
                                          char* out_job_id,
                                          size_t out_cap);

// Get generation job progress.
esp_err_t timelapseManagerJobProgress(const char* job_id,
                                      uint32_t* out_pct,
                                      bool* out_done);

// Delete a project (and its files).
esp_err_t timelapseManagerDeleteProject(const char* project_id);

// Download project output (AVI bytes via HTTP handler).
// Returns the file path and size for the handler to stream.
esp_err_t timelapseManagerGetOutputPath(const char* project_id,
                                        char* out_path, size_t out_cap,
                                        uint32_t* out_size);

// Preview project output (first N frames as MJPEG).
esp_err_t timelapseManagerPreviewProject(const char* project_id,
                                         uint32_t start_frame,
                                         uint32_t frame_count);

// ---- Internal: called by timelapse task -----------------------------------

// Capture one frame on schedule. Called from the timelapse task.
esp_err_t timelapseCaptureFrame();

// ---- Time sync gate -------------------------------------------------------

// Returns true if wall clock is synchronised (safe to timestamp media).
bool timelapseTimeSynchronised();

}  // namespace visioncam
