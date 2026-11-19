// playback.cpp - server-side MJPEG replay (Phase 3 task 3.4; SPEC 4.6).
//
// Reads an AVI segment from SD in sector-multiple blocks, walks the 00dc
// chunks, and streams frames to the browser as a timed
// multipart/x-mixed-replace MJPEG response at the recorded FPS. Seeking is
// by restarting at a frame offset from idx1 (start_frame / start_ms param).
//
// Concurrency rule: playback runs at lower priority than the storage writer
// and is allowed only within a configured share of measured card throughput;
// otherwise returns 503 PLAYBACK_BUSY. Playback consumes one stream slot.

#include "playback.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "avi_writer.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "storage_manager.h"
#include "wall_clock.h"

#include "recording_manager.h"

namespace visioncam {

static const char* TAG = "vc_play";

// ---- Module state --------------------------------------------------------
static bool g_initialized = false;

// ---- Playback session (one active at a time) -----------------------------
struct PlaybackSession {
    bool active = false;
    httpd_req_t* req = nullptr;
    FILE* file = nullptr;
    char recording_id[24] = {};
    uint32_t total_frames = 0;
    uint32_t frame_index = 0;
    uint32_t measured_fps = 0;
    uint32_t movi_offset = 0;   // offset of movi data start
    uint32_t idx1_offset = 0;   // offset of idx1 in file
    uint32_t idx1_count = 0;
    uint8_t read_buf[8192];     // sector-aligned read buffer
    uint32_t buf_len = 0;
    uint32_t buf_pos = 0;
};

static PlaybackSession g_session;

// ---- Helpers -------------------------------------------------------------

static uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static bool findFourcc(FILE* f, uint32_t fourcc, uint32_t* out_offset) {
    // Scan file for a fourcc marker. Returns offset of the fourcc.
    uint8_t buf[4];
    while (std::fread(buf, 1, 4, f) == 4) {
        if (readLe32(buf) == fourcc) {
            *out_offset = static_cast<uint32_t>(std::ftell(f)) - 4;
            return true;
        }
        // Step back 3 bytes to handle overlapping markers:
        std::fseek(f, -3, SEEK_CUR);
    }
    return false;
}

static bool readChunkHeader(FILE* f, uint32_t* out_fourcc, uint32_t* out_size,
                            uint32_t* out_offset) {
    uint8_t hdr[8];
    if (std::fread(hdr, 1, 8, f) != 8) return false;
    *out_offset = static_cast<uint32_t>(std::ftell(f)) - 8;
    *out_fourcc = readLe32(hdr + 0);
    *out_size = readLe32(hdr + 4);
    return true;
}

// ---- Public API ----------------------------------------------------------

esp_err_t playbackManagerInit() {
    g_initialized = true;
    std::memset(&g_session, 0, sizeof(g_session));
    ESP_LOGI(TAG, "playback manager init");
    return ESP_OK;
}

PlaybackResult playbackCanStart(const char* /*recording_id*/) {
    // TODO: check if recording exists and is complete, check throughput share
    if (g_session.active) return PlaybackResult::Busy;
    return PlaybackResult::Ok;
}

esp_err_t playbackStreamStart(httpd_req_t* req, const char* recording_id,
                              uint32_t /*start_frame*/,
                              uint32_t /*start_ms*/) {
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    if (g_session.active) return ESP_ERR_INVALID_STATE;

    // Look up recording:
    RecordingInfo info{};
    if (recordingManagerLookup(recording_id, &info) != ESP_OK)
        return ESP_ERR_NOT_FOUND;

    // Open the AVI file:
    g_session.file = std::fopen(info.path, "r");
    if (g_session.file == nullptr) return ESP_ERR_NOT_FOUND;

    // Find movi and idx1:
    uint32_t movi_off = 0, idx1_off = 0;
    if (!findFourcc(g_session.file, kFourccMovi, &movi_off)) {
        std::fclose(g_session.file);
        g_session.file = nullptr;
        return ESP_ERR_INVALID_STATE;
    }
    // movi fourcc is at movi_off, then 4-byte size, then "movi" form type:
    std::fseek(g_session.file, movi_off + 8, SEEK_SET); // past LIST header
    // Now at first chunk:
    g_session.movi_offset = static_cast<uint32_t>(std::ftell(g_session.file));

    if (!findFourcc(g_session.file, kFourccIdx1, &idx1_off)) {
        // No idx1 - will walk sequentially
        g_session.idx1_offset = 0;
        g_session.idx1_count = 0;
    } else {
        g_session.idx1_offset = idx1_off;
        // Read idx1 size:
        std::fseek(g_session.file, idx1_off + 4, SEEK_SET);
        uint8_t size_buf[4];
        if (std::fread(size_buf, 1, 4, g_session.file) == 4) {
            g_session.idx1_count = readLe32(size_buf) / kIdxEntryBytes;
        }
        g_session.total_frames = g_session.idx1_count;
    }

    g_session.active = true;
    g_session.req = req;
    g_session.frame_index = 0;
    g_session.measured_fps = info.fps;
    std::strncpy(g_session.recording_id, recording_id,
                 sizeof(g_session.recording_id) - 1);

    // Set response headers for multipart/x-mixed-replace:
    httpd_resp_set_type(req, "multipart/x-mixed-replace");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    const char* boundary = "--frameboundary";
    httpd_resp_set_hdr(req, "Content-Type",
                       std::string("multipart/x-mixed-replace; boundary=")
                           .append(boundary)
                           .c_str());

    ESP_LOGI(TAG, "playback start: %s (%u frames, %u fps)", recording_id,
             g_session.total_frames, g_session.measured_fps);
    return ESP_OK;
}

PlaybackResult playbackStreamFrame(httpd_req_t* /*req*/) {
    if (!g_session.active || g_session.file == nullptr)
        return PlaybackResult::Error;

    // If we've sent all frames, stop:
    if (g_session.total_frames > 0 &&
        g_session.frame_index >= g_session.total_frames) {
        return PlaybackResult::Stopped;
    }

    // Walk to next 00dc chunk:
    uint32_t fourcc = 0, size = 0, offset = 0;
    if (!readChunkHeader(g_session.file, &fourcc, &size, &offset)) {
        return PlaybackResult::Stopped;
    }

    if (fourcc != kFourcc00dc) {
        // Skip non-00dc chunks (e.g. idx1):
        std::fseek(g_session.file, aviPad4(size), SEEK_CUR);
        return PlaybackResult::Ok; // try again for next chunk
    }

    // Read the JPEG data:
    std::vector<uint8_t> jpeg(size);
    if (std::fread(jpeg.data(), 1, size, g_session.file) != size) {
        return PlaybackResult::Error;
    }

    // Skip padding to 4-byte boundary:
    uint32_t padded = aviPad4(size);
    if (padded > size) {
        std::fseek(g_session.file, padded - size, SEEK_CUR);
    }

    // Send multipart boundary + headers + JPEG:
    char boundary_hdr[128];
    int hdr_len = std::snprintf(boundary_hdr, sizeof(boundary_hdr),
                                "--frameboundary\r\n"
                                "Content-Type: image/jpeg\r\n"
                                "Content-Length: %u\r\n"
                                "\r\n",
                                static_cast<unsigned int>(size));
    if (httpd_resp_send_chunk(g_session.req, boundary_hdr,
                              hdr_len) != ESP_OK) {
        return PlaybackResult::Error;
    }
    if (httpd_resp_send_chunk(g_session.req,
                              reinterpret_cast<const char*>(jpeg.data()),
                              size) != ESP_OK) {
        return PlaybackResult::Error;
    }

    g_session.frame_index++;

    // Pace at measured FPS:
    if (g_session.measured_fps > 0) {
        uint32_t period_ms = 1000u / g_session.measured_fps;
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }

    return PlaybackResult::Ok;
}

void playbackStreamEnd(httpd_req_t* /*req*/) {
    if (g_session.file != nullptr) {
        std::fclose(g_session.file);
        g_session.file = nullptr;
    }
    g_session.active = false;
    g_session.req = nullptr;
    g_session.frame_index = 0;
    g_session.total_frames = 0;
    g_session.buf_len = 0;
    g_session.buf_pos = 0;
}

esp_err_t playbackStatus(PlaybackStatus* /*out*/) {
    // TODO: fill in playback status
    return ESP_OK;
}

uint32_t playbackMaxReadBytesPerSec() {
    // TODO: derive from measured card write speed * share
    return 500 * 1024; // 500 KB/s placeholder
}

}  // namespace visioncam
