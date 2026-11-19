// recording_manager.cpp - recording state machine (Phase 3 tasks 3.1-3.3;
// SPEC section 4.6).
//
// Recording is segment-based: MJPEG AVI segments, default 60 s each.
// The writer task (prio 5) is the sole file writer for recordings - it
// pulls frame leases from its bounded queue, copies JPEG data to an 8 KB
// staging buffer, releases the lease immediately (never holds across
// blocking I/O), and writes 00dc chunks to SD in whole-buffer writes.

#include "recording_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "avi_writer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "storage_manager.h"
#include "tasks.h"
#include "wall_clock.h"

namespace visioncam {

static const char* TAG = "vc_rec";

// ---- NVS namespace for recording config (SPEC 7: versioned, namespaced) --
constexpr char kRecNvsNs[] = "recording";
constexpr char kRecKeyVer[] = "rec_ver";
constexpr char kRecKeyEnabled[] = "rec_enabled";
constexpr char kRecKeyFps[] = "rec_fps";
constexpr char kRecKeyWidth[] = "rec_width";
constexpr char kRecKeyHeight[] = "rec_height";
constexpr char kRecKeySegDur[] = "rec_segdur";
constexpr char kRecKeyMinDur[] = "rec_mindur";
constexpr char kRecKeyMaxPct[] = "rec_maxpct";
constexpr char kRecKeyPostEvent[] = "rec_postevent";
constexpr char kRecKeyPreEvent[] = "rec_preevent";
constexpr uint32_t kRecConfigSchema = 1;

// ---- Directory layout (SPEC section 4.8) ---------------------------------
constexpr char kDirRecording[] = "/sdcard/recording";
constexpr char kDirContinuous[] = "/sdcard/recording/continuous";
constexpr char kDirEvents[] = "/sdcard/recording/events";
constexpr char kDirManual[] = "/sdcard/recording/manual";
constexpr char kDirIndex[] = "/sdcard/system/index";
constexpr char kRecordingIndexPath[] = "/sdcard/system/index/recording-index.jsonl";

// ---- Bounds (AGENTS rules 12-13) -----------------------------------------
constexpr uint32_t kRecordingMaxSegments = 128;
constexpr size_t kRecordingIndexMaxBytes =
    kRecordingMaxSegments * 256;   // ~128 KB PSRAM ceiling
constexpr uint32_t kRecordingQueueDepth = 2;   // drop-oldest (writer task)

// ---- Module state --------------------------------------------------------
static RecordingState g_state = RecordingState::Idle;
static RecordingConfig g_config{};
static FrameSubscriberId g_sub_id = 0;   // 0 = not subscribed
static TaskHandle_t g_writer_task = nullptr;

// Current segment (only one open at a time):
static char g_cur_rec_id[24] = {};
static char g_cur_temp_path[128] = {};
static char g_cur_final_path[128] = {};
static FILE* g_cur_file = nullptr;
static uint32_t g_cur_start_time = 0;
static uint32_t g_cur_frame_count = 0;
static uint32_t g_cur_movi_size = 0;
static uint32_t g_cur_width = 0;
static uint32_t g_cur_height = 0;
static uint32_t g_cur_category = 0;   // 0=continuous, 1=manual, 2=event
static bool g_cur_event_marked = false;
static uint8_t* g_cur_idx1_psram = nullptr;
static uint32_t g_cur_idx1_count = 0;

// Staging buffer for the writer task (8 KB, sector-aligned):
static uint8_t g_cur_stage[kAviStageBytes];
static uint32_t g_cur_stage_offset = 0;

// Recording index (in-memory cache, bounded):
static RecordingInfo g_rec_index[kRecordingMaxSegments] = {};
static uint32_t g_rec_index_count = 0;
static uint32_t g_rec_next_id = 1;

// ---- Helpers -------------------------------------------------------------

static uint32_t wallClockNow() {
    return static_cast<uint32_t>(wallClockSeconds());
}

static const char* categoryName(uint32_t cat) {
    switch (cat) {
        case 0: return "continuous";
        case 1: return "manual";
        case 2: return "event";
        default: return "manual";
    }
}

// framesize_t -> dimensions (standard resolutions, written fresh):
static void framesizeToDimensions(framesize_t fs, uint32_t* w,
                                  uint32_t* h) {
    switch (fs) {
        case FRAMESIZE_QQVGA: *w = 160; *h = 120; break;
        case FRAMESIZE_HQVGA: *w = 240; *h = 176; break;
        case FRAMESIZE_QVGA:  *w = 320; *h = 240; break;
        case FRAMESIZE_CIF:   *w = 400; *h = 296; break;
        case FRAMESIZE_VGA:   *w = 640; *h = 480; break;
        case FRAMESIZE_SVGA:  *w = 800; *h = 600; break;
        case FRAMESIZE_XGA:   *w = 1024; *h = 768; break;
        case FRAMESIZE_SXGA:  *w = 1280; *h = 1024; break;
        case FRAMESIZE_UXGA:  *w = 1600; *h = 1200; break;
        default:              *w = 640; *h = 480; break;
    }
}

static esp_err_t allocateRecordingId(char* out_id, size_t out_cap) {
    if (out_id == nullptr || out_cap < 24) return ESP_ERR_INVALID_ARG;
    if (g_rec_next_id >= kRecordingMaxSegments) return ESP_ERR_NO_MEM;
    int n = std::snprintf(out_id, out_cap, "rec-%08u",
                          static_cast<unsigned int>(g_rec_next_id++));
    return (n > 0 && static_cast<size_t>(n) < out_cap) ? ESP_OK
                                                       : ESP_ERR_INVALID_SIZE;
}

// ---- Segment file operations ---------------------------------------------

static esp_err_t segmentOpen(const char* rec_id, uint32_t category,
                             bool event_marked, uint32_t width,
                             uint32_t height, uint32_t start_time) {
    if (g_cur_file != nullptr) return ESP_ERR_INVALID_STATE;

    // Build temp path:
    // /sdcard/recording/<category>/YYYY/MM/DD/rec-XXXXXX.tmp
    const char* cat_dir = (category == 0) ? kDirContinuous :
                          (category == 2) ? kDirEvents : kDirManual;

    time_t now_t = static_cast<time_t>(start_time);
    std::tm tm{};
    gmtime_r(&now_t, &tm);
    char date_dir[64];
    std::snprintf(date_dir, sizeof(date_dir), "%04d/%02d/%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    int n = std::snprintf(g_cur_temp_path, sizeof(g_cur_temp_path),
                          "%s/%s/%s.tmp", cat_dir, date_dir, rec_id);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(g_cur_temp_path))
        return ESP_ERR_INVALID_SIZE;

    n = std::snprintf(g_cur_final_path, sizeof(g_cur_final_path),
                      "%s/%s/%s.avi", cat_dir, date_dir, rec_id);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(g_cur_final_path))
        return ESP_ERR_INVALID_SIZE;

    // Write provisional header (reserves space at offset 0):
    uint8_t header[kAviHeaderBytes];
    size_t hdr_len = aviBuildHeader(width, height, g_config.capture_fps,
                                    header, sizeof(header));
    if (hdr_len == 0) return ESP_ERR_INVALID_SIZE;

    // Allocate PSRAM index buffer:
    g_cur_idx1_psram = static_cast<uint8_t*>(
        heap_caps_malloc(kAviIndexMaxBytes, MALLOC_CAP_SPIRAM));
    if (g_cur_idx1_psram == nullptr) return ESP_ERR_NO_MEM;
    // Write idx1 header (8 bytes) at start of PSRAM buffer:
    std::memcpy(g_cur_idx1_psram, "idx1", 4);
    std::uint32_t zero = 0;
    std::memcpy(g_cur_idx1_psram + 4, &zero, 4);  // size patched at close

    // Write temp file with provisional header:
    esp_err_t err = storageWriteFile(g_cur_temp_path, header, hdr_len);
    if (err != ESP_OK) {
        heap_caps_free(g_cur_idx1_psram);
        g_cur_idx1_psram = nullptr;
        return err;
    }

    // Open file for appending (after the header):
    g_cur_file = std::fopen(g_cur_temp_path, "a");
    if (g_cur_file == nullptr) {
        heap_caps_free(g_cur_idx1_psram);
        g_cur_idx1_psram = nullptr;
        return ESP_ERR_NOT_FOUND;
    }
    std::fseek(g_cur_file, static_cast<long>(hdr_len), SEEK_SET);

    std::strncpy(g_cur_rec_id, rec_id, sizeof(g_cur_rec_id) - 1);
    g_cur_rec_id[sizeof(g_cur_rec_id) - 1] = '\0';
    g_cur_start_time = start_time;
    g_cur_frame_count = 0;
    g_cur_movi_size = hdr_len;  // include header size
    g_cur_width = width;
    g_cur_height = height;
    g_cur_category = category;
    g_cur_event_marked = event_marked;
    g_cur_idx1_count = 0;
    g_cur_stage_offset = 0;

    // Record in index as "writing":
    RecordingInfo& info =
        g_rec_index[g_rec_index_count % kRecordingMaxSegments];
    std::memset(&info, 0, sizeof(info));
    std::strncpy(info.recording_id, rec_id, sizeof(info.recording_id) - 1);
    std::strncpy(info.path, g_cur_final_path, sizeof(info.path) - 1);
    std::strncpy(info.category, categoryName(category),
                 sizeof(info.category) - 1);
    info.start_time = start_time;
    info.width = width;
    info.height = height;
    info.fps = g_config.capture_fps;
    std::strncpy(info.status, "writing", sizeof(info.status) - 1);
    g_rec_index_count++;

    ESP_LOGI(TAG, "segment open: %s (%s, %lux%u, %u fps)", rec_id,
             categoryName(category), width, height,
             static_cast<unsigned int>(g_config.capture_fps));
    return ESP_OK;
}

static esp_err_t segmentWriteChunk(const uint8_t* data, uint32_t len,
                                   uint32_t /*offset_in_movi*/) {
    if (g_cur_file == nullptr) return ESP_ERR_INVALID_STATE;

    // Build 00dc chunk:
    uint8_t chunk[kChunkHeaderBytes + kAviStageBytes];
    size_t chunk_len =
        aviBuild00dcChunk(data, len, chunk, sizeof(chunk));
    if (chunk_len == 0) return ESP_ERR_INVALID_SIZE;

    if (std::fwrite(chunk, 1, chunk_len, g_cur_file) != chunk_len)
        return ESP_ERR_INVALID_STATE;

    g_cur_movi_size += chunk_len;

    // Update idx1 entry in PSRAM:
    if (g_cur_idx1_count < kAviMaxFramesPerSegment) {
        uint8_t* entry =
            g_cur_idx1_psram + 8 + g_cur_idx1_count * kIdxEntryBytes;
        aviBuildIdxEntry(data, g_cur_movi_size - chunk_len, len, entry);
        g_cur_idx1_count++;
    }

    g_cur_frame_count++;
    return ESP_OK;
}

static esp_err_t segmentClose() {
    if (g_cur_file == nullptr) {
        ESP_LOGE(TAG, "segment close: g_cur_file is null");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "segment close: starting, frames=%u movi_size=%u",
             g_cur_frame_count, g_cur_movi_size);

    // Finalize header:
    uint32_t now = wallClockNow();
    uint32_t duration = (now >= g_cur_start_time) ? (now - g_cur_start_time) : 0;
    uint32_t measured_fps = (duration > 0)
        ? (g_cur_frame_count * 1000u / duration)
        : g_config.capture_fps;

    AviHeaderFields fields{};
    fields.dwMicroSecPerFrame = measured_fps ? (1000000u / measured_fps) : 0;
    fields.dwTotalFrames = g_cur_frame_count;
    fields.dwWidth = g_cur_width;
    fields.dwHeight = g_cur_height;
    fields.dwRate = measured_fps;
    fields.dwLength = g_cur_frame_count;
    fields.dwSuggestedBufferSize = g_cur_width * g_cur_height;

    uint8_t header_patch[kAviHeaderBytes];
    // Patch idx1 size in PSRAM buffer:
    std::uint32_t idx1_size = g_cur_idx1_count * kIdxEntryBytes;
    std::memcpy(g_cur_idx1_psram + 4, &idx1_size, 4);
    size_t hdr_size = aviFinalizeHeader(fields, g_cur_movi_size,
                                        header_patch, sizeof(header_patch),
                                        g_cur_idx1_psram, kAviIndexMaxBytes);
    if (hdr_size == 0) {
        std::fclose(g_cur_file);
        g_cur_file = nullptr;
        return ESP_ERR_INVALID_SIZE;
    }

    // Rewrite header at offset 0:
    std::fseek(g_cur_file, 0, SEEK_SET);
    if (std::fwrite(header_patch, 1, hdr_size, g_cur_file) != hdr_size) {
        ESP_LOGE(TAG, "segment close: header write failed");
        std::fclose(g_cur_file);
        g_cur_file = nullptr;
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "segment close: header written (%u bytes)", hdr_size);

    // Append idx1 index:
    if (std::fwrite(g_cur_idx1_psram, 1, 8 + idx1_size, g_cur_file) !=
        8 + idx1_size) {
        ESP_LOGE(TAG, "segment close: idx1 write failed");
        std::fclose(g_cur_file);
        g_cur_file = nullptr;
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "segment close: idx1 written (%u bytes)", 8 + idx1_size);

    if (std::fclose(g_cur_file) != 0) {
        ESP_LOGE(TAG, "segment close: fclose failed");
        g_cur_file = nullptr;
        return ESP_ERR_INVALID_STATE;
    }
    g_cur_file = nullptr;
    ESP_LOGI(TAG, "segment close: fclose OK");

    // Record in index as "ready":
    for (uint32_t i = 0; i < g_rec_index_count; i++) {
        if (std::strcmp(g_rec_index[i].recording_id, g_cur_rec_id) == 0) {
            g_rec_index[i].duration_s = duration;
            g_rec_index[i].frame_count = g_cur_frame_count;
            g_rec_index[i].size_bytes =
                g_cur_movi_size + hdr_size + 8 + idx1_size;
            g_rec_index[i].fps = measured_fps;
            std::strncpy(g_rec_index[i].status, "ready",
                         sizeof(g_rec_index[i].status) - 1);
            break;
        }
    }

    ESP_LOGI(TAG, "segment close: %s (%u frames, %u bytes, %u fps, %u s)",
             g_cur_rec_id, g_cur_frame_count,
             static_cast<unsigned int>(g_cur_movi_size + hdr_size + 8 +
                                       idx1_size),
             measured_fps, duration);

    // Reset current segment:
    g_cur_rec_id[0] = '\0';
    g_cur_temp_path[0] = '\0';
    g_cur_final_path[0] = '\0';
    heap_caps_free(g_cur_idx1_psram);
    g_cur_idx1_psram = nullptr;
    g_cur_movi_size = 0;
    g_cur_idx1_count = 0;
    g_cur_stage_offset = 0;

    return ESP_OK;
}

// ---- Writer task ---------------------------------------------------------

static void writerTask(void* /*param*/) {
    FrameLease lease{};

    while (true) {
        // If not recording, park:
        if (g_state != RecordingState::Recording) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Acquire a frame lease (block up to 100 ms):
        if (!frameAcquire(g_sub_id, lease, 100)) {
            continue;
        }

        // Copy JPEG to staging buffer, then release lease immediately:
        if (lease.len > 0 && lease.len <= kAviStageBytes) {
            // If staging buffer doesn't have room, flush first:
            if (g_cur_stage_offset + lease.len > kAviStageBytes) {
                if (g_cur_file != nullptr && g_cur_stage_offset > 0) {
                    segmentWriteChunk(g_cur_stage, g_cur_stage_offset,
                                      g_cur_movi_size);
                }
                g_cur_stage_offset = 0;
            }
            if (g_cur_file != nullptr) {
                std::memcpy(g_cur_stage + g_cur_stage_offset, lease.data,
                            lease.len);
                g_cur_stage_offset += lease.len;
            }
        }

        // Release lease immediately (never hold across I/O):
        frameRelease(lease);

        // Check if segment should rotate:
        if (g_cur_file != nullptr) {
            uint32_t now = wallClockNow();
            uint32_t duration =
                (now >= g_cur_start_time) ? (now - g_cur_start_time) : 0;
            bool rotate = false;

            // Duration cap:
            if (g_config.segment_duration_s > 0 &&
                duration >= g_config.segment_duration_s) {
                rotate = true;
            }
            // Frame cap:
            if (g_cur_frame_count >= kAviMaxFramesPerSegment) {
                rotate = true;
            }

            if (rotate) {
                segmentClose();
                g_cur_stage_offset = 0;
                // Re-open a new segment if continuous recording enabled:
                if (g_config.enabled &&
                    g_state == RecordingState::Recording) {
                    char new_id[24] = {};
                    if (allocateRecordingId(new_id, sizeof(new_id)) ==
                        ESP_OK) {
                        segmentOpen(new_id, g_cur_category, g_cur_event_marked,
                                    g_cur_width, g_cur_height,
                                    wallClockNow());
                    }
                }
            }
        }
    }
}

// ---- Public API ----------------------------------------------------------

esp_err_t recordingManagerInit(const RecordingConfig* config) {
    if (config != nullptr) g_config = *config;

    // Load config from NVS:
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kRecNvsNs, NVS_READONLY, &h);
    if (err == ESP_OK) {
        struct Guard {
            nvs_handle_t hh;
            ~Guard() { nvs_close(hh); }
        } guard{h};
        uint8_t en = 0;
        uint32_t v = 0;
        if (nvs_get_u32(h, kRecKeyVer, &v) == ESP_OK && v == kRecConfigSchema) {
            nvs_get_u8(h, kRecKeyEnabled, &en);
            g_config.enabled = (en != 0);
            nvs_get_u32(h, kRecKeyFps, &g_config.capture_fps);
            nvs_get_u32(h, kRecKeyWidth, &g_config.width);
            nvs_get_u32(h, kRecKeyHeight, &g_config.height);
            nvs_get_u32(h, kRecKeySegDur, &g_config.segment_duration_s);
            nvs_get_u32(h, kRecKeyMinDur, &g_config.min_duration_s);
            nvs_get_u32(h, kRecKeyMaxPct, &g_config.max_storage_percent);
            nvs_get_u32(h, kRecKeyPostEvent, &g_config.post_event_s);
            nvs_get_u32(h, kRecKeyPreEvent, &g_config.pre_event_s);
        }
    }

    // Subscribe to frame manager:
    err = frameManagerSubscribe(&g_sub_id, "recorder");
    if (err != ESP_OK) return err;

    // Create writer task:
    if (xTaskCreate(writerTask, "vc_writer", VC_STACK_WRITER, nullptr,
                    VC_PRIO_WRITER, &g_writer_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    // Run crash recovery scanner for *.tmp files:
    err = recordingManagerRecoverTmpSegments();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tmp segment recovery: %s", esp_err_to_name(err));
    }

    g_state = RecordingState::Idle;
    ESP_LOGI(TAG, "recording manager init: fps=%u segdur=%u",
             static_cast<unsigned int>(g_config.capture_fps),
             static_cast<unsigned int>(g_config.segment_duration_s));
    return ESP_OK;
}

esp_err_t recordingManagerShutdown() {
    g_state = RecordingState::Idle;
    if (g_writer_task != nullptr) {
        vTaskDelete(g_writer_task);
        g_writer_task = nullptr;
    }
    if (g_cur_file != nullptr) {
        std::fclose(g_cur_file);
        g_cur_file = nullptr;
    }
    if (g_cur_idx1_psram != nullptr) {
        heap_caps_free(g_cur_idx1_psram);
        g_cur_idx1_psram = nullptr;
    }
    return ESP_OK;
}

esp_err_t recordingManagerStartManual(char* out_id, size_t out_cap) {
    if (g_state != RecordingState::Idle) return ESP_ERR_INVALID_STATE;
    if (out_id == nullptr || out_cap < 24) return ESP_ERR_INVALID_ARG;

    esp_err_t err = allocateRecordingId(out_id, out_cap);
    if (err != ESP_OK) return err;

    uint32_t now = wallClockNow();
    uint32_t w, h;
    framesizeToDimensions(g_config.frame_size, &w, &h);
    err = segmentOpen(out_id, 1, false,   // category=manual, not event-marked
                      w, h, now);
    if (err != ESP_OK) return err;

    g_state = RecordingState::Recording;
    return ESP_OK;
}

esp_err_t recordingManagerStop() {
    if (g_state != RecordingState::Recording) return ESP_ERR_INVALID_STATE;
    esp_err_t err = segmentClose();
    g_state = RecordingState::Idle;
    return err;
}

esp_err_t recordingManagerState(RecordingState* out) {
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = g_state;
    return ESP_OK;
}

esp_err_t recordingManagerGetConfig(RecordingConfig* out) {
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = g_config;
    return ESP_OK;
}

esp_err_t recordingManagerSetConfig(const RecordingConfig* candidate,
                                    RecordingConfig* effective) {
    if (candidate == nullptr || effective == nullptr)
        return ESP_ERR_INVALID_ARG;

    // Candidate starts at current config:
    RecordingConfig new_cfg = g_config;

    // Validate ranges:
    if (candidate->capture_fps > 0) {
        if (candidate->capture_fps > 60) return ESP_ERR_INVALID_ARG;
        new_cfg.capture_fps = candidate->capture_fps;
    }
    if (candidate->segment_duration_s > 0) {
        if (candidate->segment_duration_s > 3600) return ESP_ERR_INVALID_ARG;
        new_cfg.segment_duration_s = candidate->segment_duration_s;
    }
    if (candidate->min_duration_s > 0) {
        if (candidate->min_duration_s > 3600) return ESP_ERR_INVALID_ARG;
        new_cfg.min_duration_s = candidate->min_duration_s;
    }
    if (candidate->max_storage_percent > 0) {
        if (candidate->max_storage_percent > 100) return ESP_ERR_INVALID_ARG;
        new_cfg.max_storage_percent = candidate->max_storage_percent;
    }
    if (candidate->post_event_s > 0) {
        new_cfg.post_event_s = candidate->post_event_s;
    }
    if (candidate->pre_event_s > 0) {
        if (candidate->pre_event_s > 60) return ESP_ERR_INVALID_ARG;
        new_cfg.pre_event_s = candidate->pre_event_s;
    }
    new_cfg.enabled = candidate->enabled;

    // Persist to NVS:
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kRecNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    struct Guard {
        nvs_handle_t hh;
        ~Guard() { nvs_close(hh); }
    } guard{h};
    nvs_set_u32(h, kRecKeyVer, kRecConfigSchema);
    nvs_set_u8(h, kRecKeyEnabled, new_cfg.enabled ? 1 : 0);
    nvs_set_u32(h, kRecKeyFps, new_cfg.capture_fps);
    nvs_set_u32(h, kRecKeyWidth, new_cfg.width);
    nvs_set_u32(h, kRecKeyHeight, new_cfg.height);
    nvs_set_u32(h, kRecKeySegDur, new_cfg.segment_duration_s);
    nvs_set_u32(h, kRecKeyMinDur, new_cfg.min_duration_s);
    nvs_set_u32(h, kRecKeyMaxPct, new_cfg.max_storage_percent);
    nvs_set_u32(h, kRecKeyPostEvent, new_cfg.post_event_s);
    nvs_set_u32(h, kRecKeyPreEvent, new_cfg.pre_event_s);
    err = nvs_commit(h);
    if (err != ESP_OK) return err;

    // Apply:
    g_config = new_cfg;
    *effective = g_config;
    return ESP_OK;
}

esp_err_t recordingManagerList(uint32_t offset, uint32_t limit,
                               RecordingInfo* out, size_t out_cap,
                               size_t* out_count, bool* out_has_more) {
    if (out == nullptr || out_count == nullptr || out_has_more == nullptr)
        return ESP_ERR_INVALID_ARG;

    *out_count = 0;
    *out_has_more = false;
    uint32_t total = 0;

    for (uint32_t i = 0; i < g_rec_index_count; i++) {
        total++;
        if (total <= offset) continue;
        if (*out_count >= limit) {
            *out_has_more = true;
            break;
        }
        if (*out_count >= out_cap) break;
        out[*out_count] = g_rec_index[i];
        (*out_count)++;
    }
    return ESP_OK;
}

esp_err_t recordingManagerLookup(const char* recording_id,
                                 RecordingInfo* out) {
    if (recording_id == nullptr || out == nullptr)
        return ESP_ERR_INVALID_ARG;
    for (uint32_t i = 0; i < g_rec_index_count; i++) {
        if (std::strcmp(g_rec_index[i].recording_id, recording_id) == 0) {
            *out = g_rec_index[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t recordingManagerDelete(const char* recording_id) {
    // Lookup first:
    RecordingInfo info{};
    esp_err_t err = recordingManagerLookup(recording_id, &info);
    if (err != ESP_OK) return err;

    // Refuse if segment is "writing" (active):
    if (std::strcmp(info.status, "writing") == 0)
        return ESP_ERR_INVALID_STATE;

    // Delete the file:
    err = storageRemoveFile(info.path);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;

    // Remove from index:
    for (uint32_t i = 0; i < g_rec_index_count; i++) {
        if (std::strcmp(g_rec_index[i].recording_id, recording_id) == 0) {
            // Move last element to this position:
            g_rec_index[i] = g_rec_index[g_rec_index_count - 1];
            std::memset(&g_rec_index[g_rec_index_count - 1], 0,
                        sizeof(g_rec_index[g_rec_index_count - 1]));
            g_rec_index_count--;
            break;
        }
    }

    ESP_LOGI(TAG, "recording deleted: %s", recording_id);
    return ESP_OK;
}

esp_err_t recordingManagerRecoverTmpSegments() {
    // Scan /sdcard/recording recursively for *.tmp files:
    // Stub for now - the full scanner walks 00dc chunks, rebuilds
    // idx1 + header, renames to final with recovered flag (SPEC 4.6 item 8).
    ESP_LOGI(TAG, "tmp segment recovery: stub (not yet implemented)");
    return ESP_OK;
}

// ---- Internal: called by writer task -------------------------------------

esp_err_t recordingWriterFrame(const FrameLease& /*lease*/) {
    // Handled inline in writerTask.
    return ESP_OK;
}

bool recordingWriterShouldRotate() {
    // Handled inline in writerTask.
    return false;
}

}  // namespace visioncam
