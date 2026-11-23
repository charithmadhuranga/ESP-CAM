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
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "avi_writer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
static uint32_t g_cur_width = 0;
static uint32_t g_cur_height = 0;
static uint32_t g_cur_category = 0;   // 0=continuous, 1=manual, 2=event
static bool g_cur_event_marked = false;
static uint64_t g_cur_first_frame_us = 0;  // esp_timer, first frame written
static uint64_t g_cur_last_frame_us = 0;   // esp_timer, last frame written
static uint32_t g_cur_movi_payload = 0;    // bytes of 00dc chunks written
static uint32_t g_cur_max_frame = 0;       // largest JPEG in this segment
// idx1 chunk (8 B header + one 16 B entry per frame), PSRAM, bounded by
// kAviMaxFramesPerSegment (rotation forces a new segment at the cap).
static uint8_t* g_cur_idx1_psram = nullptr;

// Per-frame JPEG copy (PSRAM) so the lease is released before any SD I/O
// (frame_manager.h copy pattern). Grown on demand, bounded by
// kRecFrameMaxBytes; a larger frame is dropped and counted.
constexpr uint32_t kRecFrameMaxBytes = 400 * 1024;  // > UXGA JPEG capacity
static uint8_t* g_frame_buf = nullptr;
static uint32_t g_frame_buf_cap = 0;
static uint32_t g_frames_dropped_oversize = 0;

// Serializes segment open/write/close between the writer task and the
// HTTP/API callers (start/stop), so a stop can never close the file while
// a frame write is in flight.
static SemaphoreHandle_t g_seg_mutex = nullptr;
struct SegLock {
    SegLock() { xSemaphoreTake(g_seg_mutex, portMAX_DELAY); }
    ~SegLock() { xSemaphoreGive(g_seg_mutex); }
};

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
    if (g_rec_next_id > 99999999) return ESP_ERR_NO_MEM;
    int n = std::snprintf(out_id, out_cap, "rec-%08u",
                          static_cast<unsigned int>(g_rec_next_id++));
    return (n > 0 && static_cast<size_t>(n) < out_cap) ? ESP_OK
                                                       : ESP_ERR_INVALID_SIZE;
}

static bool recordingIdValue(const char* id, uint32_t* out_value) {
    if (id == nullptr || out_value == nullptr ||
        std::strncmp(id, "rec-", 4) != 0 || std::strlen(id) != 12)
        return false;
    char* end = nullptr;
    const unsigned long value = std::strtoul(id + 4, &end, 10);
    if (end == id + 4 || *end != '\0' || value == 0 ||
        value > 99999999)
        return false;
    *out_value = static_cast<uint32_t>(value);
    return true;
}

static bool jsonStringField(const char* line, const char* key,
                            char* out, size_t out_cap) {
    char pattern[48] = {};
    const int pattern_len =
        std::snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    if (pattern_len <= 0 ||
        static_cast<size_t>(pattern_len) >= sizeof(pattern))
        return false;
    const char* value = std::strstr(line, pattern);
    if (value == nullptr) return false;
    value += pattern_len;
    const char* end = std::strchr(value, '"');
    if (end == nullptr || static_cast<size_t>(end - value) >= out_cap)
        return false;
    const size_t len = static_cast<size_t>(end - value);
    std::memcpy(out, value, len);
    out[len] = '\0';
    return true;
}

static bool jsonU32Field(const char* line, const char* key,
                        uint32_t* out_value) {
    char pattern[48] = {};
    const int pattern_len =
        std::snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if (pattern_len <= 0 ||
        static_cast<size_t>(pattern_len) >= sizeof(pattern))
        return false;
    const char* value = std::strstr(line, pattern);
    if (value == nullptr) return false;
    value += pattern_len;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || parsed > UINT32_MAX) return false;
    *out_value = static_cast<uint32_t>(parsed);
    return true;
}

static void addRecordingIndexEntry(const RecordingInfo& info) {
    if (g_rec_index_count == kRecordingMaxSegments) {
        std::memmove(g_rec_index, g_rec_index + 1,
                     (kRecordingMaxSegments - 1) * sizeof(g_rec_index[0]));
        g_rec_index_count--;
    }
    g_rec_index[g_rec_index_count++] = info;
}

static esp_err_t persistRecordingIndex() {
    constexpr size_t kMaxLineBytes = 384;
    constexpr size_t kIndexBufferBytes =
        kRecordingMaxSegments * kMaxLineBytes;
    char* buffer = static_cast<char*>(heap_caps_malloc(
        kIndexBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) return ESP_ERR_NO_MEM;

    size_t used = 0;
    for (uint32_t i = 0; i < g_rec_index_count; ++i) {
        const RecordingInfo& info = g_rec_index[i];
        if (std::strcmp(info.status, "ready") != 0) continue;
        const int written = std::snprintf(
            buffer + used, kIndexBufferBytes - used,
            "{\"recording_id\":\"%s\",\"path\":\"%s\","
            "\"category\":\"%s\",\"start_time\":%u,"
            "\"duration_s\":%u,\"frame_count\":%u,\"size_bytes\":%u,"
            "\"width\":%u,\"height\":%u,\"fps\":%u,"
            "\"recovered\":%s,\"event_marked\":%s,\"status\":\"ready\"}\n",
            info.recording_id, info.path, info.category,
            static_cast<unsigned>(info.start_time),
            static_cast<unsigned>(info.duration_s),
            static_cast<unsigned>(info.frame_count),
            static_cast<unsigned>(info.size_bytes),
            static_cast<unsigned>(info.width),
            static_cast<unsigned>(info.height),
            static_cast<unsigned>(info.fps),
            info.recovered ? "true" : "false",
            info.event_marked ? "true" : "false");
        if (written < 0 ||
            static_cast<size_t>(written) >= kIndexBufferBytes - used) {
            heap_caps_free(buffer);
            return ESP_ERR_INVALID_SIZE;
        }
        used += static_cast<size_t>(written);
    }

    const esp_err_t err = storageWriteFile(
        kRecordingIndexPath, reinterpret_cast<const uint8_t*>(buffer), used);
    heap_caps_free(buffer);
    return err;
}

static esp_err_t loadRecordingIndex() {
    StorageReadHandle reader{};
    esp_err_t err = storageOpenReadFile(kRecordingIndexPath, &reader);
    if (err == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    char line[384] = {};
    size_t line_len = 0;
    uint8_t chunk[512];
    bool overflow = false;
    for (;;) {
        size_t got = 0;
        err = storageReadOpenFile(&reader, chunk, sizeof(chunk), &got);
        if (err != ESP_OK) break;
        if (got == 0) break;

        for (size_t i = 0; i < got; ++i) {
            if (chunk[i] != '\n') {
                if (line_len + 1 >= sizeof(line)) {
                    overflow = true;
                    continue;
                }
                if (!overflow) line[line_len++] = static_cast<char>(chunk[i]);
                continue;
            }
            if (!overflow && line_len > 0) {
                line[line_len] = '\0';
                RecordingInfo info{};
                uint32_t id_value = 0;
                const bool valid =
                    jsonStringField(line, "recording_id", info.recording_id,
                                    sizeof(info.recording_id)) &&
                    recordingIdValue(info.recording_id, &id_value) &&
                    jsonStringField(line, "path", info.path,
                                    sizeof(info.path)) &&
                    std::strncmp(info.path, "/sdcard/recording/", 18) == 0 &&
                    jsonStringField(line, "category", info.category,
                                    sizeof(info.category)) &&
                    jsonStringField(line, "status", info.status,
                                    sizeof(info.status)) &&
                    std::strcmp(info.status, "ready") == 0 &&
                    jsonU32Field(line, "start_time", &info.start_time) &&
                    jsonU32Field(line, "duration_s", &info.duration_s) &&
                    jsonU32Field(line, "frame_count", &info.frame_count) &&
                    jsonU32Field(line, "size_bytes", &info.size_bytes) &&
                    jsonU32Field(line, "width", &info.width) &&
                    jsonU32Field(line, "height", &info.height) &&
                    jsonU32Field(line, "fps", &info.fps);
                // Every row that names an ID reserves it - including rows
                // ignored below - so a new recording can never reuse the
                // ID (and file name) of one already on the card.
                if (id_value >= g_rec_next_id) g_rec_next_id = id_value + 1;
                uint32_t file_size = 0;
                if (valid && storageStatFile(info.path, &file_size, nullptr) ==
                                 ESP_OK &&
                    file_size == info.size_bytes) {
                    const char* recovered =
                        std::strstr(line, "\"recovered\":true");
                    const char* event_marked =
                        std::strstr(line, "\"event_marked\":true");
                    info.recovered = recovered != nullptr;
                    info.event_marked = event_marked != nullptr;
                    addRecordingIndexEntry(info);
                } else {
                    ESP_LOGW(TAG, "ignoring invalid/stale recording index row");
                }
            }
            line_len = 0;
            overflow = false;
        }
    }
    const esp_err_t close_err = storageCloseReadFile(&reader);
    if (err != ESP_OK) return err;
    if (close_err != ESP_OK) return close_err;
    ESP_LOGI(TAG, "loaded %u recordings from SD index",
             static_cast<unsigned>(g_rec_index_count));
    return ESP_OK;
}

// ---- Segment file operations ---------------------------------------------

// rec_id is in/out: if a file for the requested ID already exists on the
// card (e.g. left by an earlier boot whose index row was lost), the next
// free ID is allocated into the buffer instead (bounded attempts).
static esp_err_t segmentOpen(char* rec_id, size_t rec_id_cap,
                             uint32_t category,
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

    constexpr int kMaxIdAttempts = 32;
    for (int attempt = 0;; ++attempt) {
        int n = std::snprintf(g_cur_temp_path, sizeof(g_cur_temp_path),
                              "%s/%s/%s.tmp", cat_dir, date_dir, rec_id);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(g_cur_temp_path))
            return ESP_ERR_INVALID_SIZE;
        n = std::snprintf(g_cur_final_path, sizeof(g_cur_final_path),
                          "%s/%s/%s.avi", cat_dir, date_dir, rec_id);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(g_cur_final_path))
            return ESP_ERR_INVALID_SIZE;
        uint32_t sz = 0;
        const bool taken =
            storageStatFile(g_cur_final_path, &sz, nullptr) == ESP_OK ||
            storageStatFile(g_cur_temp_path, &sz, nullptr) == ESP_OK;
        if (!taken) break;
        ESP_LOGW(TAG, "segment open: %s already on card - skipping ID", rec_id);
        if (attempt + 1 >= kMaxIdAttempts) return ESP_ERR_INVALID_STATE;
        const esp_err_t id_err = allocateRecordingId(rec_id, rec_id_cap);
        if (id_err != ESP_OK) return id_err;
    }

    // Provisional header (sizes 0); rebuilt in full at close.
    AviStreamInfo avi{};
    avi.width = width;
    avi.height = height;
    avi.nominal_fps = g_config.capture_fps;
    uint8_t header[kAviHeaderBytes];
    if (aviBuildHeader(avi, header, sizeof(header)) != kAviHeaderBytes)
        return ESP_ERR_INVALID_SIZE;

    g_cur_idx1_psram = static_cast<uint8_t*>(
        heap_caps_malloc(kAviIndexMaxBytes, MALLOC_CAP_SPIRAM));
    if (g_cur_idx1_psram == nullptr) return ESP_ERR_NO_MEM;

    esp_err_t err = storageWriteFile(g_cur_temp_path, header, sizeof(header));
    if (err != ESP_OK) {
        heap_caps_free(g_cur_idx1_psram);
        g_cur_idx1_psram = nullptr;
        return err;
    }

    // "r+b", NOT "a": append mode makes every write land at EOF, which
    // silently turned the header rewrite at close into an append.
    g_cur_file = std::fopen(g_cur_temp_path, "r+b");
    if (g_cur_file == nullptr ||
        std::fseek(g_cur_file, 0, SEEK_END) != 0) {
        if (g_cur_file != nullptr) std::fclose(g_cur_file);
        g_cur_file = nullptr;
        heap_caps_free(g_cur_idx1_psram);
        g_cur_idx1_psram = nullptr;
        return ESP_ERR_NOT_FOUND;
    }

    std::strncpy(g_cur_rec_id, rec_id, sizeof(g_cur_rec_id) - 1);
    g_cur_rec_id[sizeof(g_cur_rec_id) - 1] = '\0';
    g_cur_start_time = start_time;
    g_cur_frame_count = 0;
    g_cur_width = width;
    g_cur_height = height;
    g_cur_category = category;
    g_cur_event_marked = event_marked;
    g_cur_first_frame_us = 0;
    g_cur_last_frame_us = 0;
    g_cur_movi_payload = 0;
    g_cur_max_frame = 0;

    // Record in index as "writing":
    RecordingInfo info{};
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
    addRecordingIndexEntry(info);

    ESP_LOGI(TAG, "segment open: %s (%s, %lux%u, %u fps)", rec_id,
             categoryName(category), width, height,
             static_cast<unsigned int>(g_config.capture_fps));
    return ESP_OK;
}

// Appends one JPEG as one '00dc' chunk and records its idx1 entry.
static esp_err_t segmentWriteFrame(const uint8_t* jpeg, uint32_t len,
                                   uint64_t ts_us) {
    if (g_cur_file == nullptr) return ESP_ERR_INVALID_STATE;
    if (g_cur_frame_count >= kAviMaxFramesPerSegment)
        return ESP_ERR_INVALID_SIZE;

    uint8_t ch[kChunkHeaderBytes];
    aviBuildFrameChunkHeader(len, ch);
    const uint8_t pad = 0;
    const bool needs_pad = aviPadEven(len) != len;
    if (std::fwrite(ch, 1, sizeof(ch), g_cur_file) != sizeof(ch) ||
        std::fwrite(jpeg, 1, len, g_cur_file) != len ||
        (needs_pad && std::fwrite(&pad, 1, 1, g_cur_file) != 1)) {
        return ESP_FAIL;
    }

    aviBuildIdxEntry(4 + g_cur_movi_payload, len,
                     g_cur_idx1_psram + kChunkHeaderBytes +
                         g_cur_frame_count * kIdxEntryBytes);
    g_cur_movi_payload += kChunkHeaderBytes + aviPadEven(len);
    if (len > g_cur_max_frame) g_cur_max_frame = len;
    if (g_cur_frame_count == 0) g_cur_first_frame_us = ts_us;
    g_cur_last_frame_us = ts_us;
    g_cur_frame_count++;
    return ESP_OK;
}

// Drops the current segment's index row (it never became a playable file).
static void removeCurrentIndexRow() {
    for (uint32_t i = 0; i < g_rec_index_count; i++) {
        if (std::strcmp(g_rec_index[i].recording_id, g_cur_rec_id) == 0) {
            g_rec_index[i] = g_rec_index[g_rec_index_count - 1];
            std::memset(&g_rec_index[g_rec_index_count - 1], 0,
                        sizeof(g_rec_index[0]));
            g_rec_index_count--;
            break;
        }
    }
}

// Releases the per-segment state after the file is closed (or abandoned).
static void segmentReset() {
    g_cur_rec_id[0] = '\0';
    g_cur_temp_path[0] = '\0';
    g_cur_final_path[0] = '\0';
    heap_caps_free(g_cur_idx1_psram);
    g_cur_idx1_psram = nullptr;
    g_cur_frame_count = 0;
    g_cur_movi_payload = 0;
    g_cur_max_frame = 0;
}

static esp_err_t segmentClose() {
    if (g_cur_file == nullptr) {
        ESP_LOGE(TAG, "segment close: no open segment");
        return ESP_ERR_INVALID_STATE;
    }

    // Duration from frame timestamps (ms precision). The span between the
    // first and last frame covers frames-1 intervals; add one average
    // interval so frames / duration is the true rate.
    const uint32_t frames = g_cur_frame_count;
    uint32_t duration_ms = 0;
    if (frames > 1 && g_cur_last_frame_us > g_cur_first_frame_us) {
        const uint64_t span_ms =
            (g_cur_last_frame_us - g_cur_first_frame_us) / 1000u;
        duration_ms = static_cast<uint32_t>(span_ms + span_ms / (frames - 1));
    }
    const uint32_t fps = (duration_ms > 0)
        ? static_cast<uint32_t>((frames * 1000ull + duration_ms / 2) / duration_ms)
        : g_config.capture_fps;

    AviStreamInfo avi{};
    avi.width = g_cur_width;
    avi.height = g_cur_height;
    avi.frames = frames;
    avi.duration_ms = duration_ms;
    avi.nominal_fps = g_config.capture_fps;
    avi.max_frame_bytes = g_cur_max_frame;
    avi.movi_payload_bytes = g_cur_movi_payload;
    uint8_t header[kAviHeaderBytes];
    aviBuildHeader(avi, header, sizeof(header));
    aviBuildIdx1Header(frames, g_cur_idx1_psram);
    const size_t idx1_len = kChunkHeaderBytes + frames * kIdxEntryBytes;
    const uint32_t file_bytes = aviFileBytes(g_cur_movi_payload, frames);

    // idx1 at EOF, then the final header at offset 0.
    bool ok = std::fseek(g_cur_file, 0, SEEK_END) == 0 &&
              std::fwrite(g_cur_idx1_psram, 1, idx1_len, g_cur_file) == idx1_len &&
              std::fseek(g_cur_file, 0, SEEK_SET) == 0 &&
              std::fwrite(header, 1, sizeof(header), g_cur_file) == sizeof(header);
    if (std::fclose(g_cur_file) != 0) ok = false;
    g_cur_file = nullptr;
    if (!ok) {
        ESP_LOGE(TAG, "segment close: finalize write failed (%s left as .tmp)",
                 g_cur_rec_id);
        removeCurrentIndexRow();
        persistRecordingIndex();
        segmentReset();
        return ESP_FAIL;
    }

    if (frames == 0) {
        // Nothing captured: do not publish an empty, unplayable recording.
        storageRemoveFile(g_cur_temp_path);
        removeCurrentIndexRow();
        ESP_LOGW(TAG, "segment close: %s had no frames - discarded",
                 g_cur_rec_id);
        segmentReset();
        return persistRecordingIndex();
    }

    esp_err_t rename_err = storageRenameFile(g_cur_temp_path, g_cur_final_path);
    if (rename_err != ESP_OK) {
        ESP_LOGE(TAG, "segment close: failed to finalize recording file");
        removeCurrentIndexRow();
        persistRecordingIndex();
        segmentReset();
        return rename_err;
    }

    const uint32_t duration_s = (duration_ms + 500u) / 1000u;
    for (uint32_t i = 0; i < g_rec_index_count; i++) {
        if (std::strcmp(g_rec_index[i].recording_id, g_cur_rec_id) == 0) {
            g_rec_index[i].duration_s = duration_s;
            g_rec_index[i].frame_count = frames;
            g_rec_index[i].size_bytes = file_bytes;
            g_rec_index[i].fps = fps;
            g_rec_index[i].width = g_cur_width;
            g_rec_index[i].height = g_cur_height;
            std::strncpy(g_rec_index[i].status, "ready",
                         sizeof(g_rec_index[i].status) - 1);
            break;
        }
    }
    const esp_err_t index_err = persistRecordingIndex();
    if (index_err != ESP_OK) {
        ESP_LOGE(TAG, "segment close: recording index persist failed: %s",
                 esp_err_to_name(index_err));
    }

    ESP_LOGI(TAG, "segment close: %s (%u frames, %u B, %u fps, %u ms, %ux%u)",
             g_cur_rec_id, static_cast<unsigned>(frames),
             static_cast<unsigned>(file_bytes), static_cast<unsigned>(fps),
             static_cast<unsigned>(duration_ms),
             static_cast<unsigned>(g_cur_width),
             static_cast<unsigned>(g_cur_height));
    segmentReset();
    return index_err;
}

// ---- Writer task ---------------------------------------------------------

// Grows the PSRAM frame copy buffer to hold `need` bytes (bounded).
static bool frameBufReserve(uint32_t need) {
    if (need <= g_frame_buf_cap) return true;
    if (need > kRecFrameMaxBytes) return false;
    const uint32_t cap = std::min<uint32_t>((need + 16383u) & ~16383u,
                                            kRecFrameMaxBytes);
    uint8_t* p = static_cast<uint8_t*>(
        heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (p == nullptr) return false;
    heap_caps_free(g_frame_buf);
    g_frame_buf = p;
    g_frame_buf_cap = cap;
    return true;
}

static void writerTask(void* /*param*/) {
    FrameLease lease{};

    while (true) {
        if (g_state != RecordingState::Recording) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!frameAcquire(g_sub_id, lease, 100)) continue;

        // Copy out and release the lease before any SD I/O (rule 9).
        const uint32_t len = lease.len;
        const uint64_t ts_us = lease.ts_us;
        const uint16_t fw = lease.width;
        const uint16_t fh = lease.height;
        bool have_frame = false;
        if (len > 0 && frameBufReserve(len)) {
            have_frame = frameLeaseCopyTo(lease, g_frame_buf, g_frame_buf_cap) == ESP_OK;
        } else if (len > 0) {
            g_frames_dropped_oversize++;
        }
        frameRelease(lease);
        if (!have_frame) continue;

        SegLock lock;
        if (g_state != RecordingState::Recording || g_cur_file == nullptr)
            continue;  // stopped while we were copying

        // Header dimensions come from the frames actually recorded, not
        // the configured frame size (live view may have changed it).
        if (g_cur_frame_count == 0 && fw > 0 && fh > 0) {
            g_cur_width = fw;
            g_cur_height = fh;
        }

        if (segmentWriteFrame(g_frame_buf, len, ts_us) != ESP_OK) {
            ESP_LOGE(TAG, "frame write failed - closing segment");
            segmentClose();
            g_state = RecordingState::Idle;
            continue;
        }

        const uint32_t now = wallClockNow();
        const uint32_t elapsed =
            (now >= g_cur_start_time) ? (now - g_cur_start_time) : 0;
        const bool rotate =
            (g_config.segment_duration_s > 0 &&
             elapsed >= g_config.segment_duration_s) ||
            g_cur_frame_count >= kAviMaxFramesPerSegment;
        if (!rotate) continue;

        // Segment boundary: close and keep recording into a new segment
        // until the caller stops (manual/event) or forever (continuous).
        segmentClose();
        char new_id[24] = {};
        if (allocateRecordingId(new_id, sizeof(new_id)) != ESP_OK ||
            segmentOpen(new_id, sizeof(new_id), g_cur_category, g_cur_event_marked,
                        g_cur_width, g_cur_height, wallClockNow()) != ESP_OK) {
            ESP_LOGE(TAG, "segment rotate: could not open next segment");
            g_state = RecordingState::Idle;
        }
    }
}

// ---- Public API ----------------------------------------------------------

esp_err_t recordingManagerInit(const RecordingConfig* config) {
    if (config != nullptr) g_config = *config;

    const esp_err_t index_err = loadRecordingIndex();
    if (index_err != ESP_OK) {
        ESP_LOGE(TAG, "recording index load failed: %s",
                 esp_err_to_name(index_err));
        return index_err;
    }

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

    if (g_seg_mutex == nullptr) g_seg_mutex = xSemaphoreCreateMutex();
    if (g_seg_mutex == nullptr) return ESP_ERR_NO_MEM;

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
    // Unsubscribe from the frame manager so the subscriber slot is
    // released instead of leaking across start/stop cycles. Without this
    // every init/shutdown pair consumes one of the VC_FRAME_SUBSCRIBERS
    // slots until the registry is full and new subscribers (snapshot,
    // live stream) can no longer register.
    if (g_sub_id != 0) {
        frameManagerUnsubscribe(g_sub_id);
        g_sub_id = 0;
    }
    if (g_cur_file != nullptr) {
        std::fclose(g_cur_file);
        g_cur_file = nullptr;
    }
    if (g_cur_idx1_psram != nullptr) {
        heap_caps_free(g_cur_idx1_psram);
        g_cur_idx1_psram = nullptr;
    }
    heap_caps_free(g_frame_buf);
    g_frame_buf = nullptr;
    g_frame_buf_cap = 0;
    return ESP_OK;
}

esp_err_t recordingManagerStartManual(char* out_id, size_t out_cap) {
    if (out_id == nullptr || out_cap < 24) return ESP_ERR_INVALID_ARG;
    if (g_seg_mutex == nullptr) return ESP_ERR_INVALID_STATE;
    SegLock lock;
    if (g_state != RecordingState::Idle) return ESP_ERR_INVALID_STATE;

    esp_err_t err = allocateRecordingId(out_id, out_cap);
    if (err != ESP_OK) return err;

    uint32_t now = wallClockNow();
    uint32_t w, h;
    framesizeToDimensions(g_config.frame_size, &w, &h);
    err = segmentOpen(out_id, out_cap, 1, false,   // manual, not event-marked
                      w, h, now);
    if (err != ESP_OK) return err;

    g_state = RecordingState::Recording;
    return ESP_OK;
}

esp_err_t recordingManagerStop() {
    if (g_seg_mutex == nullptr) return ESP_ERR_INVALID_STATE;
    SegLock lock;
    if (g_state != RecordingState::Recording) return ESP_ERR_INVALID_STATE;
    g_state = RecordingState::Idle;
    if (g_cur_file == nullptr) return ESP_OK;  // already closed by the writer
    return segmentClose();
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
    ESP_LOGW(TAG, "recording id not found: %s (%u indexed)",
             recording_id, static_cast<unsigned>(g_rec_index_count));
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
    err = persistRecordingIndex();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "recording deleted but index update failed: %s",
                 esp_err_to_name(err));
        return err;
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
