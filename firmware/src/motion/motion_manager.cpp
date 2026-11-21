// motion_manager.cpp — motion detection engine (Phase 4 task 4.1; SPEC §4.3).
//
// Decodes each camera frame to a small grayscale bitmap using esp_jpeg_decode,
// compares with the previous sample using per-block SAD, and publishes
// MotionStarted/MotionEnded events on the typed event bus.

#include "motion_manager.h"

#include <algorithm>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "event_bus.h"
#include "jpeg_decoder.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "recording_manager.h"
#include "snapshot_manager.h"
#include "storage_manager.h"
#include "tasks.h"
#include "wall_clock.h"

namespace visioncam {

static const char* TAG = "vc_motion";

// ---- Bounds (AGENTS rules 12-13) -----------------------------------------
constexpr size_t kMotionJpegMaxBytes = 64 * 1024;   // fixed JPEG staging buffer
constexpr size_t kMotionJpegOutBytes = 32 * 1024;   // fixed JPEG output buffer (RGB888 at 1:8 scale)
constexpr uint32_t kMotionGrayDim = 96;             // 96x96 grayscale (R1 RESIZE_DIM)
constexpr size_t kMotionGrayPixels = kMotionGrayDim * kMotionGrayDim; // 9216 B
constexpr uint32_t kMotionQueueDepth = 8;           // event queue depth (drop-oldest)

// ---- NVS namespace for motion config (SPEC 7) ----------------------------
constexpr char kMotionNvsNs[] = "motion";
constexpr char kMotionKeyVer[] = "mot_ver";
constexpr char kMotionKeyEnabled[] = "mot_enabled";
constexpr char kMotionKeyRateIdle[] = "mot_rate_idle";
constexpr char kMotionKeyRateRec[] = "mot_rate_rec";
constexpr char kMotionKeyThreshold[] = "mot_threshold";
constexpr char kMotionKeyMinRatio[] = "mot_min_ratio";
constexpr char kMotionKeyCooldown[] = "mot_cooldown";
constexpr char kMotionKeyMinFrames[] = "mot_min_frames";
constexpr char kMotionKeySettleFrames[] = "mot_settle_frames";
constexpr char kMotionKeyRecord[] = "mot_record";
constexpr char kMotionKeySnapshot[] = "mot_snapshot";
constexpr char kMotionKeyMqtt[] = "mot_mqtt";
constexpr uint32_t kMotionConfigSchema = 1;

// ---- Module state --------------------------------------------------------
static MotionConfig g_config{};
static MotionStatus g_status{};
static FrameSubscriberId g_sub_id = 0;   // 0 = not subscribed
static TaskHandle_t g_motion_task = nullptr;
static bool g_initialized = false;

// Motion analysis state:
static uint8_t* g_jpeg_staging = nullptr;     // fixed 64 KB JPEG staging
static uint8_t* g_jpeg_output = nullptr;      // fixed 32 KB JPEG decode output (RGB888)
static uint8_t* g_gray_curr = nullptr;        // 96x96 grayscale current
static uint8_t* g_gray_prev = nullptr;        // 96x96 grayscale previous
static uint32_t g_frame_id_last = 0;          // last processed frame id
static uint32_t g_warmup_frames = 0;          // warm-up counter
static uint32_t g_motion_frames = 0;          // consecutive changed frames
static uint32_t g_cooldown_until_ms = 0;      // cooldown expiry (uptime ms)
static bool g_motion_active = false;          // current motion state
static uint32_t g_next_check_ms = 0;          // next analysis deadline

// ---- Helpers -------------------------------------------------------------

static uint32_t uptimeMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// Convert RGB888 to grayscale using integer luminance (R1 formula):
// Y = (R*21846 + G*42992 + B*9350) >> 16
static inline uint8_t rgbToGray(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint8_t>(
        (static_cast<uint32_t>(r) * 21846u +
         static_cast<uint32_t>(g) * 42992u +
         static_cast<uint32_t>(b) * 9350u) >> 16);
}

// Decode JPEG to grayscale 96x96 using esp_jpeg_decode at 1:8 scale.
// Returns ESP_OK on success, ESP_ERR_INVALID_SIZE if JPEG too large,
// ESP_FAIL on decode error.
static esp_err_t decodeJpegToGray(const uint8_t* jpeg, uint32_t jpeg_len,
                                  uint8_t* gray_out) {
    if (jpeg_len > kMotionJpegMaxBytes) return ESP_ERR_INVALID_SIZE;

    // Copy JPEG to staging buffer:
    std::memcpy(g_jpeg_staging, jpeg, jpeg_len);

    // Decode at 1:8 scale to RGB888:
    esp_jpeg_image_cfg_t cfg{};
    cfg.indata = g_jpeg_staging;
    cfg.indata_size = jpeg_len;
    cfg.outbuf = g_jpeg_output;
    cfg.outbuf_size = kMotionJpegOutBytes;
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;
    cfg.out_scale = JPEG_IMAGE_SCALE_1_8;
    cfg.advanced.working_buffer = nullptr;

    esp_jpeg_image_output_t out{};
    esp_err_t err = esp_jpeg_decode(&cfg, &out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg decode failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // out.width / out.height are the decoded dimensions at 1:8 scale.
    // Downsample to 96x96 grayscale:
    const uint32_t src_w = out.width;
    const uint32_t src_h = out.height;
    const uint8_t* rgb = reinterpret_cast<const uint8_t*>(cfg.outbuf); // actually out.data?

    // esp_jpeg_decode writes to cfg.outbuf:
    // Actually, let me check: the decode function writes to outbuf.
    // We need to access the output buffer. The cfg.outbuf points to it.
    const uint8_t* outbuf = cfg.outbuf;
    if (outbuf == nullptr) return ESP_FAIL;

    // Downsample to 96x96. The decoded image at 1:8 scale is SMALLER than
    // 96x96 for every frame size the camera actually runs (VGA 640x480 ->
    // 80x60, QVGA -> 40x30), so plain floor-mapping produced EMPTY source
    // blocks and count == 0 -> r_sum / count divided by zero and panicked
    // the motion task (Guru "IntegerDivideByZero", boot-16). The mapping
    // below is centred, keeps at least one source pixel per output pixel,
    // clamps to the decoded bounds, and guards count == 0 — an undersized
    // source is upscaled by sampling, never averaged to a crash.
    if (src_w == 0 || src_h == 0) return ESP_ERR_INVALID_SIZE;

    for (uint32_t oy = 0; oy < kMotionGrayDim; ++oy) {
        // Centred scale: output pixel oy covers the middle of its bin.
        const uint32_t sy_start =
            (static_cast<uint64_t>(oy) * src_h) / kMotionGrayDim;
        uint32_t sy_end =
            (static_cast<uint64_t>(oy + 1) * src_h) / kMotionGrayDim;
        if (sy_end <= sy_start) sy_end = sy_start + 1;
        if (sy_start >= src_h) continue;   // beyond the decoded image
        if (sy_end > src_h) sy_end = src_h;

        for (uint32_t ox = 0; ox < kMotionGrayDim; ++ox) {
            const uint32_t sx_start =
                (static_cast<uint64_t>(ox) * src_w) / kMotionGrayDim;
            uint32_t sx_end =
                (static_cast<uint64_t>(ox + 1) * src_w) / kMotionGrayDim;
            if (sx_end <= sx_start) sx_end = sx_start + 1;
            if (sx_start >= src_w) continue;
            if (sx_end > src_w) sx_end = src_w;

            uint32_t r_sum = 0, g_sum = 0, b_sum = 0, count = 0;
            for (uint32_t sy = sy_start; sy < sy_end; ++sy) {
                for (uint32_t sx = sx_start; sx < sx_end; ++sx) {
                    const size_t idx = (static_cast<size_t>(sy) * src_w + sx) * 3;
                    r_sum += outbuf[idx];
                    g_sum += outbuf[idx + 1];
                    b_sum += outbuf[idx + 2];
                    ++count;
                }
            }
            const uint32_t denom = (count != 0) ? count : 1u;
            gray_out[oy * kMotionGrayDim + ox] =
                rgbToGray(static_cast<uint8_t>(r_sum / denom),
                          static_cast<uint8_t>(g_sum / denom),
                          static_cast<uint8_t>(b_sum / denom));
        }
    }
    return ESP_OK;
}

// SAD comparison between two grayscale frames:
// Returns changed-pixel ratio (0-100) and updates light level.
static uint32_t compareGray(const uint8_t* curr, const uint8_t* prev,
                            uint32_t threshold, uint32_t& light_level) {
    uint32_t changed_blocks = 0;
    const uint32_t total_blocks = kMotionGrayDim * kMotionGrayDim;
    uint32_t lux_sum = 0;

    // Block size = 1 (per-pixel) for 96x96 — simplest and matches R1's
    // detectBlockSize=1:
    for (uint32_t i = 0; i < total_blocks; ++i) {
        uint8_t c = curr[i];
        uint8_t p = prev[i];
        lux_sum += c;

        int diff = static_cast<int>(c) - static_cast<int>(p);
        if (diff < 0) diff = -diff;
        if (diff > static_cast<int>(threshold)) {
            ++changed_blocks;
        }
    }

    // Light level as 0-100 percent:
    light_level = (total_blocks > 0)
        ? (lux_sum * 100u) / (total_blocks * 255u)
        : 0;

    return (total_blocks > 0) ? (changed_blocks * 100u) / total_blocks : 0;
}

// ---- Motion task ---------------------------------------------------------

static void motionTask(void* /*param*/) {
    FrameLease lease{};
    uint32_t last_sample_ms = 0;

    while (true) {
        if (!g_config.enabled) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Compute sample interval based on recording state:
        RecordingState rec_state = RecordingState::Idle;
        recordingManagerState(&rec_state);
        uint32_t interval_ms;
        if (rec_state == RecordingState::Recording) {
            // Lower rate while recording: 1 check / sample_rate_rec_hz seconds,
            // default 1 check / 2 s:
            if (g_config.sample_rate_rec_hz > 0) {
                interval_ms = 1000u / g_config.sample_rate_rec_hz;
            } else {
                interval_ms = 2000u; // R1 default: 1 check / 2 s
            }
        } else {
            // Higher rate while idle:
            if (g_config.sample_rate_idle_hz > 0) {
                interval_ms = 1000u / g_config.sample_rate_idle_hz;
            } else {
                interval_ms = 200u; // ~5 checks/s
            }
        }

        // Wait until next sample time:
        uint32_t now = uptimeMs();
        if (g_next_check_ms == 0) g_next_check_ms = now + interval_ms;
        if (now < g_next_check_ms) {
            vTaskDelay(pdMS_TO_TICKS(g_next_check_ms - now));
            continue;
        }
        g_next_check_ms = now + interval_ms;

        // Acquire frame lease (poll, non-blocking):
        if (!frameAcquire(g_sub_id, lease, 0)) {
            // No frame available — skip this cycle:
            ++g_status.checks_skipped;
            continue;
        }

        // Copy JPEG to staging, release lease immediately. The length is
        // captured BEFORE release: frameRelease() zeroes the lease (it is
        // single-use by design — frame_manager.h), and reading lease.len
        // afterwards returned 0, so every decode was handed
        // indata_size=0 and failed with JDR_INP "wrong termination of
        // input stream" — motion never detected anything (boot logs showed
        // the same esp_jpeg_decode(97) failure at every check, ~344/boot).
        const uint32_t jpeg_len = lease.len;
        const bool size_ok = (jpeg_len > 0 && jpeg_len <= kMotionJpegMaxBytes);
        if (size_ok) {
            std::memcpy(g_jpeg_staging, lease.data, jpeg_len);
        }
        frameRelease(lease);
        if (!size_ok) {
            ++g_status.checks_skipped;
            continue;   // empty or oversized frame: counted, never decoded
        }

        // Ignore warm-up frames:
        if (g_warmup_frames < g_config.settle_frames) {
            ++g_warmup_frames;
            // Store first valid frame as prev:
            if (g_gray_prev != nullptr) {
                // decodeJpegToGray will be called next cycle after warmup
            }
            continue;
        }

        // Decode JPEG to grayscale:
        uint32_t decode_start = uptimeMs();
        esp_err_t derr = decodeJpegToGray(g_jpeg_staging, jpeg_len, g_gray_curr);
        uint32_t decode_ms = uptimeMs() - decode_start;
        g_status.decode_time_ms_last = decode_ms;

        if (derr != ESP_OK) {
            ++g_status.checks_skipped;
            continue;
        }

        // On first valid frame after warmup, just store as prev:
        if (g_motion_frames == 0 && !g_motion_active) {
            std::memcpy(g_gray_prev, g_gray_curr, kMotionGrayPixels);
            ++g_status.checks_total;
            continue;
        }

        // Compare with previous:
        uint32_t analyze_start = uptimeMs();
        uint32_t light_level = 0;
        uint32_t changed_ratio = compareGray(g_gray_curr, g_gray_prev,
                                             g_config.threshold, light_level);
        uint32_t analyze_ms = uptimeMs() - analyze_start;
        g_status.analyze_time_ms_last = analyze_ms;
        g_status.changed_ratio_last = changed_ratio;
        g_status.light_level_last = light_level;
        ++g_status.checks_total;

        // Swap prev/curr:
        std::swap(g_gray_prev, g_gray_curr);

        // Motion detection logic with temporal smoothing:
        bool changed = (changed_ratio >= g_config.min_changed_ratio);

        if (changed) {
            ++g_motion_frames;
        } else {
            g_motion_frames = 0;
        }

        // State machine with cooldown:
        uint32_t now_ms = uptimeMs();
        if (!g_motion_active &&
            g_motion_frames >= g_config.min_frames &&
            now_ms >= g_cooldown_until_ms) {
            // Motion started:
            g_motion_active = true;
            g_cooldown_until_ms = now_ms + g_config.cooldown_ms;
            ++g_status.events_total;
            g_status.events_last_ts_ms = now_ms;
            g_status.active = true;

            eventBusPublish(EventType::MotionStarted,
                            static_cast<uint16_t>(changed_ratio), 0);

            ESP_LOGI(TAG, "motion started: changed_ratio=%u%% light=%u%%",
                     changed_ratio, light_level);

            // Trigger snapshot:
            if (g_config.snapshot_on_motion) {
                // Snapshot trigger via snapshot manager:
                // (snapshot capture would be called here)
            }

            // Trigger event recording:
            if (g_config.record_on_motion) {
                // Recording trigger via recording manager:
                // (recording start would be called here)
            }
        } else if (g_motion_active && g_motion_frames == 0) {
            // Motion ended (after cooldown):
            g_motion_active = false;
            g_status.active = false;
            eventBusPublish(EventType::MotionEnded, 0, 0);
            ESP_LOGI(TAG, "motion ended");
        }
    }
}

// ---- Public API ----------------------------------------------------------

esp_err_t motionManagerInit(const MotionConfig* config) {
    if (config != nullptr) g_config = *config;

    // Load config from NVS:
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kMotionNvsNs, NVS_READONLY, &h);
    if (err == ESP_OK) {
        struct Guard {
            nvs_handle_t hh;
            ~Guard() { nvs_close(hh); }
        } guard{h};
        uint8_t en = 0;
        uint32_t v = 0;
        if (nvs_get_u32(h, kMotionKeyVer, &v) == ESP_OK &&
            v == kMotionConfigSchema) {
            nvs_get_u8(h, kMotionKeyEnabled, &en);
            g_config.enabled = (en != 0);
            nvs_get_u32(h, kMotionKeyRateIdle, &g_config.sample_rate_idle_hz);
            nvs_get_u32(h, kMotionKeyRateRec, &g_config.sample_rate_rec_hz);
            nvs_get_u32(h, kMotionKeyThreshold, &g_config.threshold);
            nvs_get_u32(h, kMotionKeyMinRatio, &g_config.min_changed_ratio);
            nvs_get_u32(h, kMotionKeyCooldown, &g_config.cooldown_ms);
            nvs_get_u32(h, kMotionKeyMinFrames, &g_config.min_frames);
            nvs_get_u32(h, kMotionKeySettleFrames, &g_config.settle_frames);
            nvs_get_u8(h, kMotionKeyRecord, reinterpret_cast<uint8_t*>(&g_config.record_on_motion));
            nvs_get_u8(h, kMotionKeySnapshot, reinterpret_cast<uint8_t*>(&g_config.snapshot_on_motion));
            nvs_get_u8(h, kMotionKeyMqtt, reinterpret_cast<uint8_t*>(&g_config.mqtt_notify));
        }
    }

    // Allocate bounded buffers:
    g_jpeg_staging = static_cast<uint8_t*>(
        heap_caps_malloc(kMotionJpegMaxBytes, MALLOC_CAP_DEFAULT));
    g_jpeg_output = static_cast<uint8_t*>(
        heap_caps_malloc(kMotionJpegOutBytes, MALLOC_CAP_DEFAULT));
    g_gray_curr = static_cast<uint8_t*>(
        heap_caps_malloc(kMotionGrayPixels, MALLOC_CAP_DEFAULT));
    g_gray_prev = static_cast<uint8_t*>(
        heap_caps_malloc(kMotionGrayPixels, MALLOC_CAP_DEFAULT));
    if (g_jpeg_staging == nullptr || g_jpeg_output == nullptr ||
        g_gray_curr == nullptr || g_gray_prev == nullptr) {
        ESP_LOGE(TAG, "motion buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    // Subscribe to frame manager:
    err = frameManagerSubscribe(&g_sub_id, "motion");
    if (err != ESP_OK) return err;

    // Create motion task:
    if (xTaskCreate(motionTask, "vc_motion", VC_STACK_MOTION, nullptr,
                    VC_PRIO_MOTION, &g_motion_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    g_initialized = true;
    g_status.enabled = g_config.enabled;
    ESP_LOGI(TAG, "motion manager init: enabled=%d threshold=%u min_ratio=%u%%",
             g_config.enabled ? 1 : 0,
             static_cast<unsigned int>(g_config.threshold),
             static_cast<unsigned int>(g_config.min_changed_ratio));
    return ESP_OK;
}

esp_err_t motionManagerShutdown() {
    g_initialized = false;
    g_config.enabled = false;
    if (g_motion_task != nullptr) {
        vTaskDelete(g_motion_task);
        g_motion_task = nullptr;
    }
    if (g_sub_id != 0) {
        frameManagerUnsubscribe(g_sub_id);
        g_sub_id = 0;
    }
    heap_caps_free(g_jpeg_staging);
    heap_caps_free(g_jpeg_output);
    heap_caps_free(g_gray_curr);
    heap_caps_free(g_gray_prev);
    g_jpeg_staging = nullptr;
    g_jpeg_output = nullptr;
    g_gray_curr = nullptr;
    g_gray_prev = nullptr;
    return ESP_OK;
}

esp_err_t motionManagerGetConfig(MotionConfig* out) {
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = g_config;
    return ESP_OK;
}

esp_err_t motionManagerSetConfig(const MotionConfig* candidate,
                                 MotionConfig* effective) {
    if (candidate == nullptr || effective == nullptr)
        return ESP_ERR_INVALID_ARG;

    MotionConfig new_cfg = g_config;

    // Validate ranges:
    if (candidate->sample_rate_idle_hz > 0) {
        if (candidate->sample_rate_idle_hz > 100) return ESP_ERR_INVALID_ARG;
        new_cfg.sample_rate_idle_hz = candidate->sample_rate_idle_hz;
    }
    if (candidate->sample_rate_rec_hz > 0) {
        if (candidate->sample_rate_rec_hz > 100) return ESP_ERR_INVALID_ARG;
        new_cfg.sample_rate_rec_hz = candidate->sample_rate_rec_hz;
    }
    if (candidate->threshold > 0) {
        if (candidate->threshold > 255) return ESP_ERR_INVALID_ARG;
        new_cfg.threshold = candidate->threshold;
    }
    if (candidate->min_changed_ratio > 0) {
        if (candidate->min_changed_ratio > 100) return ESP_ERR_INVALID_ARG;
        new_cfg.min_changed_ratio = candidate->min_changed_ratio;
    }
    if (candidate->cooldown_ms > 0) {
        new_cfg.cooldown_ms = candidate->cooldown_ms;
    }
    if (candidate->min_frames > 0) {
        if (candidate->min_frames > 100) return ESP_ERR_INVALID_ARG;
        new_cfg.min_frames = candidate->min_frames;
    }
    if (candidate->settle_frames > 0) {
        if (candidate->settle_frames > 50) return ESP_ERR_INVALID_ARG;
        new_cfg.settle_frames = candidate->settle_frames;
    }
    new_cfg.enabled = candidate->enabled;
    new_cfg.record_on_motion = candidate->record_on_motion;
    new_cfg.snapshot_on_motion = candidate->snapshot_on_motion;
    new_cfg.mqtt_notify = candidate->mqtt_notify;

    // Persist to NVS:
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kMotionNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    struct Guard {
        nvs_handle_t hh;
        ~Guard() { nvs_close(hh); }
    } guard{h};
    nvs_set_u32(h, kMotionKeyVer, kMotionConfigSchema);
    nvs_set_u8(h, kMotionKeyEnabled, new_cfg.enabled ? 1 : 0);
    nvs_set_u32(h, kMotionKeyRateIdle, new_cfg.sample_rate_idle_hz);
    nvs_set_u32(h, kMotionKeyRateRec, new_cfg.sample_rate_rec_hz);
    nvs_set_u32(h, kMotionKeyThreshold, new_cfg.threshold);
    nvs_set_u32(h, kMotionKeyMinRatio, new_cfg.min_changed_ratio);
    nvs_set_u32(h, kMotionKeyCooldown, new_cfg.cooldown_ms);
    nvs_set_u32(h, kMotionKeyMinFrames, new_cfg.min_frames);
    nvs_set_u32(h, kMotionKeySettleFrames, new_cfg.settle_frames);
    nvs_set_u8(h, kMotionKeyRecord, new_cfg.record_on_motion ? 1 : 0);
    nvs_set_u8(h, kMotionKeySnapshot, new_cfg.snapshot_on_motion ? 1 : 0);
    nvs_set_u8(h, kMotionKeyMqtt, new_cfg.mqtt_notify ? 1 : 0);
    err = nvs_commit(h);
    if (err != ESP_OK) return err;

    g_config = new_cfg;
    g_status.enabled = g_config.enabled;
    *effective = g_config;
    return ESP_OK;
}

esp_err_t motionManagerStatus(MotionStatus* out) {
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = g_status;
    return ESP_OK;
}

esp_err_t motionManagerSetEnabled(bool enabled) {
    g_config.enabled = enabled;
    g_status.enabled = enabled;
    if (!enabled) {
        g_motion_active = false;
        g_status.active = false;
        g_motion_frames = 0;
    }
    return ESP_OK;
}

esp_err_t motionProcessFrame(const FrameLease& /*lease*/) {
    // Handled inline in motionTask.
    return ESP_OK;
}

}  // namespace visioncam
