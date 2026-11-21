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
constexpr char kPlaybackBoundary[] = "frameboundary";

// ---- Module state --------------------------------------------------------
static bool g_initialized = false;

// ---- Playback session (one active at a time) -----------------------------
struct PlaybackSession {
    bool active = false;
    httpd_req_t* req = nullptr;
    StorageReadHandle reader{};
    char recording_id[24] = {};
    uint32_t total_frames = 0;
    uint32_t frame_index = 0;
    uint32_t measured_fps = 0;
    uint32_t file_size = 0;
    uint32_t file_offset = 0;
    uint8_t read_buf[8192];
};

static PlaybackSession g_session;

// ---- Helpers -------------------------------------------------------------

static uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
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

    RecordingInfo info{};
    if (recordingManagerLookup(recording_id, &info) != ESP_OK)
        return ESP_ERR_NOT_FOUND;
    if (info.size_bytes < kAviHeaderBytes || info.frame_count == 0)
        return ESP_ERR_INVALID_STATE;

    esp_err_t err = storageOpenReadFile(info.path, &g_session.reader);
    if (err != ESP_OK) return err;
    g_session.file_size = info.size_bytes;

    uint8_t header[512] = {};
    const size_t header_len =
        std::min(static_cast<size_t>(info.size_bytes), sizeof(header));
    size_t header_read = 0;
    err = storageReadOpenFile(&g_session.reader, header, header_len,
                              &header_read);
    if (err != ESP_OK || header_read != header_len) {
        playbackStreamEnd(req);
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }

    uint32_t movi_offset = UINT32_MAX;
    for (size_t i = 0; i + sizeof(uint32_t) <= header_read; ++i) {
        if (readLe32(header + i) == kFourccMovi) {
            movi_offset = static_cast<uint32_t>(i);
            break;
        }
    }
    if (movi_offset == UINT32_MAX || movi_offset > info.size_bytes - 4) {
        playbackStreamEnd(req);
        return ESP_ERR_INVALID_STATE;
    }

    // The chunk walker skips the writer's zero-sized 8-byte movi placeholder.
    g_session.file_offset = movi_offset + sizeof(uint32_t);
    err = storageSeekReadFile(&g_session.reader, g_session.file_offset);
    if (err != ESP_OK) {
        playbackStreamEnd(req);
        return err;
    }
    g_session.total_frames = info.frame_count;
    g_session.frame_index = 0;
    g_session.measured_fps = info.fps;
    g_session.req = req;
    std::strncpy(g_session.recording_id, recording_id,
                 sizeof(g_session.recording_id) - 1);
    g_session.active = true;

    httpd_resp_set_type(
        req, "multipart/x-mixed-replace; boundary=frameboundary");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    ESP_LOGI(TAG, "playback start: %s (%u frames, %u fps)", recording_id,
             g_session.total_frames, g_session.measured_fps);
    return ESP_OK;
}

PlaybackResult playbackStreamFrame(httpd_req_t* req) {
    if (!g_session.active || g_session.reader.backend_handle == nullptr)
        return PlaybackResult::Error;

    if (g_session.frame_index >= g_session.total_frames) {
        static constexpr char end_boundary[] = "--frameboundary--\r\n";
        if (httpd_resp_send_chunk(req, end_boundary,
                                  sizeof(end_boundary) - 1) != ESP_OK) {
            return PlaybackResult::Stopped;
        }
        return PlaybackResult::Stopped;
    }

    if (g_session.file_offset > g_session.file_size ||
        g_session.file_size - g_session.file_offset < 8)
        return PlaybackResult::Error;

    uint8_t chunk_header[8] = {};
    size_t header_read = 0;
    if (storageReadOpenFile(&g_session.reader, chunk_header,
                            sizeof(chunk_header), &header_read) != ESP_OK ||
        header_read != sizeof(chunk_header)) {
        return PlaybackResult::Error;
    }
    const uint32_t fourcc = readLe32(chunk_header);
    const uint32_t size = readLe32(chunk_header + 4);
    g_session.file_offset += sizeof(chunk_header);
    if (fourcc != kFourcc00dc) {
        const uint32_t padded = aviPad4(size);
        if (padded > g_session.file_size - g_session.file_offset)
            return PlaybackResult::Error;
        g_session.file_offset += padded;
        if (storageSeekReadFile(&g_session.reader,
                                g_session.file_offset) != ESP_OK)
            return PlaybackResult::Error;
        return PlaybackResult::Ok;
    }

    if (size == 0 || size > g_session.file_size - g_session.file_offset) {
        return PlaybackResult::Error;
    }

    const uint32_t padded = aviPad4(size);
    if (padded > g_session.file_size - g_session.file_offset)
        return PlaybackResult::Error;

    char boundary_hdr[128];
    int hdr_len = std::snprintf(boundary_hdr, sizeof(boundary_hdr),
                                "--%s\r\n"
                                "Content-Type: image/jpeg\r\n"
                                "Content-Length: %u\r\n"
                                "\r\n",
                                kPlaybackBoundary,
                                static_cast<unsigned int>(size));
    if (hdr_len <= 0 ||
        static_cast<size_t>(hdr_len) >= sizeof(boundary_hdr))
        return PlaybackResult::Error;
    if (httpd_resp_send_chunk(g_session.req, boundary_hdr,
                              hdr_len) != ESP_OK) {
        return PlaybackResult::Error;
    }
    uint32_t jpeg_remaining = size;
    while (jpeg_remaining > 0) {
        const size_t want = std::min(static_cast<size_t>(jpeg_remaining),
                                     sizeof(g_session.read_buf));
        size_t got = 0;
        if (storageReadOpenFile(&g_session.reader, g_session.read_buf, want,
                                &got) != ESP_OK ||
            got != want) {
            return PlaybackResult::Error;
        }
        if (httpd_resp_send_chunk(
                g_session.req,
                reinterpret_cast<const char*>(g_session.read_buf), got) !=
            ESP_OK) {
            return PlaybackResult::Error;
        }
        jpeg_remaining -= static_cast<uint32_t>(got);
    }
    g_session.file_offset += padded;
    if (storageSeekReadFile(&g_session.reader, g_session.file_offset) !=
        ESP_OK)
        return PlaybackResult::Error;

    static constexpr char frame_end[] = "\r\n";
    if (httpd_resp_send_chunk(g_session.req, frame_end,
                              sizeof(frame_end) - 1) != ESP_OK) {
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
    if (g_session.reader.backend_handle != nullptr) {
        const esp_err_t err = storageCloseReadFile(&g_session.reader);
        if (err != ESP_OK) ESP_LOGE(TAG, "failed to close playback file");
    }
    g_session.active = false;
    g_session.req = nullptr;
    g_session.frame_index = 0;
    g_session.total_frames = 0;
    g_session.file_size = 0;
    g_session.file_offset = 0;
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
