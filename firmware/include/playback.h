// playback.h — server-side MJPEG replay (Phase 3 task 3.4; SPEC §4.6).
//
// Reads an AVI segment from SD in sector-multiple blocks, walks the 00dc
// chunks, and streams frames to the browser as a timed
// multipart/x-mixed-replace MJPEG response at the recorded FPS. Seeking is
// by restarting at a frame offset from idx1 (start_frame / start_ms param).
//
// Concurrency rule (deliberate divergence from R1): playback must NEVER
// stop or stall recording. Playback runs at lower priority than the
// storage writer (prio 4 vs 5) and is allowed only within a configured
// share of measured card throughput; otherwise returns 503 PLAYBACK_BUSY.
// Playback consumes one of the limited stream slots.
//
// The browser renders in an img/canvas element — no client-side AVI parser
// needed for the first release (SPEC §4.6 playback path decision).

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_http_server.h"

namespace visioncam {

// Opaque recording ID prefix (defined in storage_backend.h).

// Playback result codes.
enum class PlaybackResult : uint8_t {
    Ok,            // playback completed normally
    Stopped,       // stopped by client disconnect or new recording
    Busy,          // 503 PLAYBACK_BUSY — throughput share exceeded
    NotFound,      // recording ID unknown
    NotReady,      // segment still writing / SD not mounted
    Error,         // other failure
};

// ---- Public API ----------------------------------------------------------

// Initialise playback subsystem. Call after storage manager init.
esp_err_t playbackManagerInit();

// Check whether playback can start for the given recording. Returns
// PlaybackResult::Busy if the throughput share check fails (recording has
// priority), NotReady if the segment isn't complete, NotFound if unknown.
PlaybackResult playbackCanStart(const char* recording_id);

// Start server-side MJPEG replay for an HTTP request. The handler calls
// playbackStreamFrame() repeatedly until it returns PlaybackResult != Ok.
// The request must remain open for the duration of playback (one stream
// slot). Call playbackStreamEnd() on every exit path (RAII).
//
// start_frame / start_ms: optional seek point. If both 0, starts from
// the beginning.
esp_err_t playbackStreamStart(httpd_req_t* req, const char* recording_id,
                              uint32_t start_frame, uint32_t start_ms);

// Stream the next chunk of MJPEG data. Returns PlaybackResult::Ok while
// more data follows, PlaybackResult::Stopped when complete, or an error
// code on failure. The caller must call playbackStreamEnd() on return
// of any non-Ok result.
PlaybackResult playbackStreamFrame(httpd_req_t* req);

// End playback: release the stream slot, close the SD file, free buffers.
// Safe to call multiple times (idempotent).
void playbackStreamEnd(httpd_req_t* req);

// Get playback status (active recording, position, FPS, busy).
esp_err_t playbackStatus(class PlaybackStatus* out);

// Playback status info.
struct PlaybackStatus {
    bool active = false;
    char recording_id[24] = {};
    uint32_t frame_index = 0;
    uint32_t total_frames = 0;
    uint32_t measured_fps = 0;
};

// ---- Throughput share (SPEC §4.6 concurrency rule) ----------------------

// Returns the maximum bytes/sec playback may read from SD, derived from
// measured card write speed × configured share. If recording is active,
// playback is throttled to this share.
uint32_t playbackMaxReadBytesPerSec();

}  // namespace visioncam
