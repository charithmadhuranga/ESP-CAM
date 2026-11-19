// recording_manager.h — recording state machine (Phase 3 tasks 3.1-3.3;
// SPEC §4.6).
//
// Recording is segment-based: MJPEG AVI segments, default 60 s each,
// written to per-day folders under /sdcard/recording/{continuous,events,manual}/
// YYYY/MM/DD/. Every public API takes an opaque recording ID (rec-…),
// never a filesystem path (AGENTS rule 15, SPEC 6.0).
//
// Architecture:
//
//   capture task (prio 6)  --frame lease-->  recording queue (depth 2,
//                                              drop-oldest)
//                                                    |
//                                                    v
//                                              writer task (prio 5)
//                                                    |
//                                            copies JPEG → 8 KB staging
//                                                    |
//                                            writes 00dc chunks to SD
//                                                    |
//                                            segment rotates at duration /
//                                            size / frame cap
//
// The writer task is the SOLE file writer for recordings (AGENTS rule 10
// for storage; recording_manager is part of storage/). It never holds a
// frame lease across blocking I/O — copy then release (SPEC 4.1.1 rule 9).
//
// Segment lifecycle:
//   open:   allocate rec-… ID, create temp file (*.tmp), write provisional
//           AVI header (reserved space at offset 0)
//   write:  for each frame, append 00dc chunk + idx1 entry (PSRAM index)
//   close:  write final header + idx1, rename temp → final, record in index
//   crash:  boot-time scanner walks *.tmp files, rebuilds idx1 + header,
//           renames with recovered flag (SPEC §4.6 item 8)
//
// Bounds (AGENTS rules 12–13):
//   kRecordingMaxSegments   — bounded segment index cache
//   kRecordingIndexMaxBytes — PSRAM index ceiling
//   Queue depth 2, drop-oldest (documented next to the queue create)

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

#include "esp_err.h"
#include "frame_manager.h"

namespace visioncam {

// ---- Recording state -----------------------------------------------------

enum class RecordingState : uint8_t {
    Idle,      // not recording
    Recording, // segment open, accepting frames
    Closing,   // segment being finalized
};

// ---- Per-segment info (recorded in JSONL index) --------------------------

struct RecordingInfo {
    char recording_id[24];     // "rec-XXXXXXXX"
    char path[128];            // filesystem path (internal, never exposed)
    char category[16];         // "continuous" | "manual" | "event"
    uint32_t start_time = 0;   // epoch seconds
    uint32_t duration_s = 0;   // measured duration
    uint32_t frame_count = 0;
    uint32_t size_bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;          // measured FPS
    bool recovered = false;    // true if boot scanner rebuilt it
    bool event_marked = false; // true if motion/detection event
    char status[16] = "ready"; // "ready" | "writing" | "recovering" | "deleted"
};

// ---- Recording config ----------------------------------------------------

struct RecordingConfig {
    bool enabled = false;           // continuous recording off by default
    uint32_t capture_fps = 5;       // conservative until measured
    framesize_t frame_size = FRAMESIZE_VGA;
    uint32_t width = 640;           // derived from frame_size
    uint32_t height = 480;          // derived from frame_size
    uint32_t segment_duration_s = 60; // default 60 s (SPEC 4.6 item 8)
    uint32_t min_duration_s = 5;    // delete shorter unless event-marked
    uint32_t max_storage_percent = 85; // size retention for continuous
    // Event recording:
    uint32_t post_event_s = 10;     // record after event ends
    uint32_t pre_event_s = 0;       // 0 until RAM measured (SPEC 4.6 item 15)
};

// ---- Public API ----------------------------------------------------------

// Initialise recording subsystem. Call after storage manager init and
// frame manager start. Spawns the writer task.
esp_err_t recordingManagerInit(const RecordingConfig* config);

// Shut down: stop writer task, close any open segment.
esp_err_t recordingManagerShutdown();

// Start a manual recording segment. Returns recording_id.
esp_err_t recordingManagerStartManual(char* out_id, size_t out_cap);

// Stop the current recording segment.
esp_err_t recordingManagerStop();

// Start an event recording segment (Phase 4 will drive this via event bus).
// Returns recording_id.
esp_err_t recordingManagerStartEvent(char* out_id, size_t out_cap);

// Get current recording state.
esp_err_t recordingManagerState(RecordingState* out);

// Get config (copy).
esp_err_t recordingManagerGetConfig(RecordingConfig* out);

// Set config (parse → validate → persist → apply → return sanitised,
// SPEC §7 process). Partial updates overlay on current config.
esp_err_t recordingManagerSetConfig(const RecordingConfig* candidate,
                                    RecordingConfig* effective);

// List recordings (paginated, opaque IDs).
esp_err_t recordingManagerList(uint32_t offset, uint32_t limit,
                               RecordingInfo* out, size_t out_cap,
                               size_t* out_count, bool* out_has_more);

// Lookup recording by ID.
esp_err_t recordingManagerLookup(const char* recording_id,
                                 RecordingInfo* out);

// Delete recording by ID (and its file). Refuses if segment is "writing".
esp_err_t recordingManagerDelete(const char* recording_id);

// Get recording status: free space, active segment, etc.
esp_err_t recordingManagerStatus(class RecordingStatus* out);

// ---- Internal: called by writer task -------------------------------------

// Called by the writer task for each acquired frame lease. Copies the JPEG
// into the segment's staging buffer, updates the index, writes when the
// staging buffer is full. Returns ESP_OK on success, ESP_ERR_INVALID_STATE
// if not recording.
esp_err_t recordingWriterFrame(const FrameLease& lease);

// Called by the writer task to check if the segment should rotate
// (duration / size / frame cap reached). Returns true if rotation needed.
bool recordingWriterShouldRotate();

// ---- Crash recovery scanner (runs at boot) -------------------------------

// Scans /sdcard/recording for *.tmp segments, rebuilds idx1 + header,
// renames to final with recovered flag. Called from init().
esp_err_t recordingManagerRecoverTmpSegments();

}  // namespace visioncam
