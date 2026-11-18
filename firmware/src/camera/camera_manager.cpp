// camera_manager.cpp — see camera_manager.h.
//
// Moved here from firmware/src/camera_bringup.cpp (plan 02 task 1.1.3) so
// the camera owns its driver lifecycle as one module; the behaviour is the
// same SPEC 4.1.1 sequence Phase 0 executed and hardware-verified
// (docs/evidence/phase0-smoke-boot-11-forced-retry.log). The additions
// here are the LIVE SETTINGS API (rule 1's runtime framesize), the
// recovery primitives for task 1.1.4's ladder, and rule 8's explicit
// jpeg_buffer_size.

#include "camera_manager.h"

#include <atomic>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_profile.h"

namespace visioncam {
namespace {

const char* TAG = "vc_cam";

constexpr unsigned kMaxAttempts   = 3;    // SPEC 4.1.1 rule 5: retry up to twice
constexpr unsigned kWarmupFrames  = 3;    // SPEC 4.1.1 rule 6
constexpr uint32_t kXclkHz        = 20'000'000;  // rule 3: 20 MHz, do not exceed
constexpr int      kDefaultQuality = 12;  // 0..63, lower = better (R1's default)
constexpr framesize_t kDefaultFramesize = FRAMESIZE_VGA;  // rule 1 runtime default
// Rule 2 / §4.1.1: 2 PSRAM buffers, GRAB_LATEST (the value used below in
// buildCameraConfig). Exposed through cameraManagerFrameBufferCount()
// because the frame manager must reason about driver-buffer ownership
// against exactly this number — it is configured in one place only.
constexpr int      kFbCount = 2;

// SPEC 4.1.1 rule 8: "set jpeg_buffer_size explicitly for very small frame
// sizes". The driver default is width*height/5 (cam_hal.c:619), which
// under-provisions a tiny frame: a complex QQVGA scene at quality 12 can
// exceed 160x120/5 = 3.8 KB and get truncated. Below VGA we therefore set
// an explicit floor. At VGA and above the R2 default already has room
// (VGA: 61 KB, UXGA: 384 KB on this board), and a bigger buffer would just
// waste PSRAM, so the rule only fires where it is needed.
constexpr size_t kSmallFramesJpegBufferMin = 32u * 1024u;
constexpr framesize_t kExplicitBufferBelow = FRAMESIZE_VGA;

// Live settings: read from any task, written only by the setters (config
// path, task 1.2). Relaxed atomics — these are display/setting values,
// never a protocol state machine input.
std::atomic<framesize_t> s_framesize{kDefaultFramesize};
std::atomic<int>         s_quality{kDefaultQuality};
std::atomic<size_t>      s_jpeg_buffer_size{0};
std::atomic<bool>        s_initialised{false};
std::atomic<bool>        s_test_sabotage{false};   // task 1.1.4 smoke only

// Flush hook (cameraManagerSetFlushHook): plain function pointer, set once
// at startup before the first teardown can occur, read only from
// cameraManagerShutdown — no atomics needed for a write-once/read-many
// registration.
void (*s_flush_hook)(void) = nullptr;

// SPEC 4.1.1 rule 1: initialise at the largest frame size the PSRAM holds.
framesize_t initFrameSizeFor(size_t psram_bytes) {
    if (psram_bytes >= 3u * 1024u * 1024u) return FRAMESIZE_UXGA;  // ~4 MB
    if (psram_bytes >= 1500u * 1024u)      return FRAMESIZE_SVGA;  // ~2 MB
    return FRAMESIZE_VGA;
}

// Rule 8: the explicit buffer size for a given INIT frame size (0 = "use
// the driver default"). Uses the driver's own public resolution table, so
// no dimensions are duplicated here.
size_t explicitJpegBufferFor(framesize_t fs) {
    if (fs >= kExplicitBufferBelow) return 0;
    if (fs >= FRAMESIZE_INVALID) return 0;
    const size_t rgb_fifth =
        (static_cast<size_t>(resolution[fs].width) *
         static_cast<size_t>(resolution[fs].height)) / 5u;
    return rgb_fifth < kSmallFramesJpegBufferMin ? kSmallFramesJpegBufferMin
                                                 : rgb_fifth;
}

// SPEC 4.1.1 rules 2, 3, 8: 2 buffers in PSRAM, GRAB_LATEST, 20 MHz XCLK,
// explicit jpeg_buffer_size for small sizes (rule 8). The capacity rule 7
// checks against is derived the same way the driver derives its default.
void fillConfig(camera_config_t& cfg, framesize_t fs) {
    cfg               = camera_config_t{};
    cfg.pin_pwdn      = BOARD_CAM_PWDN;
    cfg.pin_reset     = BOARD_CAM_RESET;
    cfg.pin_xclk      = BOARD_CAM_XCLK;
    cfg.pin_sccb_sda  = BOARD_CAM_SIOD;   // sccb, not the deprecated sscb_*
    cfg.pin_sccb_scl  = BOARD_CAM_SIOC;
    cfg.pin_d7        = BOARD_CAM_Y9;
    cfg.pin_d6        = BOARD_CAM_Y8;
    cfg.pin_d5        = BOARD_CAM_Y7;
    cfg.pin_d4        = BOARD_CAM_Y6;
    cfg.pin_d3        = BOARD_CAM_Y5;
    cfg.pin_d2        = BOARD_CAM_Y4;
    cfg.pin_d1        = BOARD_CAM_Y3;
    cfg.pin_d0        = BOARD_CAM_Y2;
    cfg.pin_vsync     = BOARD_CAM_VSYNC;
    cfg.pin_href      = BOARD_CAM_HREF;
    cfg.pin_pclk      = BOARD_CAM_PCLK;
    cfg.xclk_freq_hz  = kXclkHz;
    cfg.ledc_timer    = LEDC_TIMER_0;
    cfg.ledc_channel  = LEDC_CHANNEL_0;
    cfg.pixel_format  = PIXFORMAT_JPEG;
    cfg.frame_size    = fs;
    cfg.jpeg_quality  = static_cast<uint8_t>(kDefaultQuality);
    cfg.fb_count      = kFbCount;
    cfg.fb_location   = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode     = CAMERA_GRAB_LATEST;
    cfg.jpeg_buffer_size = 0;             // filled in by the caller (rule 8)
    cfg.sccb_i2c_port = -1;
}

// SPEC 4.1.1 rule 5: power-cycle the sensor through PWDN between attempts.
// PWDN (GPIO32) is active high: assert = powered down, release = powered up.
void powerCyclePwdn() {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOARD_CAM_PWDN;
    io.mode         = GPIO_MODE_OUTPUT;
    io.pull_up_en   = GPIO_PULLUP_DISABLE;
    io.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io);

    gpio_set_level(static_cast<gpio_num_t>(BOARD_CAM_PWDN), 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(static_cast<gpio_num_t>(BOARD_CAM_PWDN), 0);
    vTaskDelay(pdMS_TO_TICKS(20));
}

sensor_t* sensorOrNull() {
    if (!s_initialised.load()) return nullptr;
    return esp_camera_sensor_get();
}

}  // namespace

bool cameraManagerInitialised() { return s_initialised.load(); }

unsigned cameraManagerFrameBufferCount() {
    return static_cast<unsigned>(kFbCount);
}

framesize_t cameraManagerFramesize() { return s_framesize.load(); }
int cameraManagerQuality() { return s_quality.load(); }

const char* cameraManagerSensorName() {
    sensor_t* sensor = sensorOrNull();
    const unsigned pid = sensor != nullptr ? sensor->id.PID : 0;
    if (pid == 0) return "none";
    for (int i = 0; i < static_cast<int>(CAMERA_MODEL_MAX); ++i) {
        if (camera_sensor[i].pid == pid && camera_sensor[i].name != nullptr) {
            return camera_sensor[i].name;
        }
    }
    return "unknown";
}

size_t cameraManagerJpegCapacity() {
    const size_t explicit_size = s_jpeg_buffer_size.load();
    if (explicit_size > 0) return explicit_size;
    const framesize_t fs = s_framesize.load();
    if (fs >= FRAMESIZE_INVALID) return 0;
    // The driver's default (cam_hal.c:619), derived from the live size.
    return (static_cast<size_t>(resolution[fs].width) *
            static_cast<size_t>(resolution[fs].height)) / 5u;
}

esp_err_t cameraManagerSetFramesize(framesize_t framesize) {
    sensor_t* sensor = sensorOrNull();
    if (sensor == nullptr) {
        ESP_LOGW(TAG, "set_framesize ignored — camera not initialised");
        return ESP_ERR_INVALID_STATE;
    }
    if (framesize >= FRAMESIZE_INVALID) {
        ESP_LOGE(TAG, "set_framesize rejected — value %d out of range",
                 static_cast<int>(framesize));
        return ESP_ERR_INVALID_ARG;
    }
    // Never above the sensor's own maximum (OV2640: UXGA).
    camera_sensor_info_t* info =
        esp_camera_sensor_get_info(&sensor->id);
    if (info != nullptr && framesize > info->max_size) {
        ESP_LOGE(TAG, "set_framesize rejected — %d exceeds the sensor maximum %d",
                 static_cast<int>(framesize), static_cast<int>(info->max_size));
        return ESP_ERR_INVALID_ARG;
    }
    if (sensor->set_framesize(sensor, framesize) != 0) {
        ESP_LOGE(TAG, "set_framesize(%d) failed on the sensor",
                 static_cast<int>(framesize));
        return ESP_FAIL;
    }
    s_framesize.store(framesize);
    ESP_LOGI(TAG, "frame size set to %d (%ux%u), jpeg capacity %u B",
             static_cast<int>(framesize),
             static_cast<unsigned>(resolution[framesize].width),
             static_cast<unsigned>(resolution[framesize].height),
             static_cast<unsigned>(cameraManagerJpegCapacity()));
    return ESP_OK;
}

esp_err_t cameraManagerSetQuality(int quality) {
    sensor_t* sensor = sensorOrNull();
    if (sensor == nullptr) {
        ESP_LOGW(TAG, "set_quality ignored — camera not initialised");
        return ESP_ERR_INVALID_STATE;
    }
    if (quality < 0 || quality > 63) {
        ESP_LOGE(TAG, "set_quality rejected — %d outside 0..63", quality);
        return ESP_ERR_INVALID_ARG;
    }
    if (sensor->set_quality(sensor, quality) != 0) {
        ESP_LOGE(TAG, "set_quality(%d) failed on the sensor", quality);
        return ESP_FAIL;
    }
    s_quality.store(quality);
    ESP_LOGI(TAG, "jpeg quality set to %d", quality);
    return ESP_OK;
}

bool cameraManagerTestSabotageArmed() { return s_test_sabotage.load(); }

void cameraManagerSetTestSabotage(bool armed) {
    s_test_sabotage.store(armed);
    if (armed) {
        ESP_LOGW(TAG, "TEST SABOTAGE ARMED — every camera init will fail "
                      "(XCLK removed) until disarmed");
    }
}

void cameraManagerSetFlushHook(void (*hook)(void)) {
    // Set once at startup (frame manager init); read from shutdown paths.
    s_flush_hook = hook;
}

void cameraManagerShutdown() {
    // esp_camera_init()'s doc comment still claims the module "can only be
    // called once and there is no way to de-initialize" (esp_camera.h:187-188).
    // That is stale in esp32-camera 2.1.9: esp_camera_deinit() is declared
    // (esp_camera.h:203), implemented (esp_camera.c:379) and nulls s_state
    // (esp_camera.c:387).
    //
    // Why every exit path calls it: a *failed* esp_camera_init already cleans
    // up after itself via its `fail:` label (esp_camera.c:374-376), but a
    // *successful* one leaves s_state set — and the PID check, warm-up and
    // malformed-frame exits in cameraManagerInit all run after a successful
    // init. Without an explicit deinit a later re-init returns
    // ESP_ERR_INVALID_STATE from camera_probe() (esp_camera.c:177-178).
    // Recorded in docs/REFERENCE_ANALYSIS.md ("Camera init / retry").
    esp_camera_deinit();
    s_initialised.store(false);
    // Flush AFTER deinit, while s_state is NULL: queued records now name
    // freed driver buffers, and fb_return inside the drain is a documented
    // no-op (esp_camera.c:410-416). This runs on EVERY camera teardown
    // (step 15, forced-failure, ladder rung 2 re-init, init-failure paths),
    // so no record can ever cross a camera lifetime.
    if (s_flush_hook != nullptr) s_flush_hook();
}

esp_err_t cameraManagerInit(CameraReport& report, CameraInitTestMode mode) {
    report = CameraReport{};

    const size_t psram = esp_psram_get_size();
    report.init_framesize = initFrameSizeFor(psram);
    // Track the init size from this moment: the rule-7 capacity check and
    // every log below must be derived from the size actually in effect, not
    // from the VGA runtime default that is applied only at the very end.
    s_framesize.store(report.init_framesize);
    ESP_LOGI(TAG, "PSRAM %u B -> init frame size %d",
             static_cast<unsigned>(psram), static_cast<int>(report.init_framesize));

    camera_config_t cfg{};
    fillConfig(cfg, report.init_framesize);
    const size_t explicit_buffer = explicitJpegBufferFor(report.init_framesize);
    cfg.jpeg_buffer_size = explicit_buffer;   // 0 = driver default (rule 8)
    s_jpeg_buffer_size.store(explicit_buffer);

    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        report.init_attempts = attempt;
        // The sticky test hook wins over the per-call mode (task 1.1.4's ladder
        // test arms it so the ladder's internal re-inits all fail).
        const bool sabotage_all =
            (mode == CameraInitTestMode::ForceAllAttempts) ||
            s_test_sabotage.load();
        const bool sabotage_first = (mode == CameraInitTestMode::ForceFirstFailure);

        if (attempt > 1) {
            cameraManagerShutdown();
            powerCyclePwdn();
            cfg.pin_xclk = (sabotage_all) ? -1 : BOARD_CAM_XCLK;
        } else if (sabotage_all || sabotage_first) {
            // Step 11: make attempt 1 genuinely fail by removing the sensor's
            // clock (pin_xclk = -1 -> GPIO0 never driven -> the SCCB probe
            // cannot see the sensor). Holding PWDN instead does NOT work: R2's
            // camera_probe releases PWDN itself at the start of every init
            // (esp_camera.c:204-216 — verified on hardware 2026-11-15).
            cfg.pin_xclk = -1;
            ESP_LOGW(TAG, "test sabotage (mode %d, sticky %d): XCLK removed — "
                          "attempt %u %s",
                     static_cast<int>(mode), s_test_sabotage.load() ? 1 : 0,
                     attempt, sabotage_all ? "fails (all attempts)"
                                           : "fails (then recovers)");
        }
        err = esp_camera_init(&cfg);
        if (err == ESP_OK) break;
        ESP_LOGE(TAG, "esp_camera_init failed (attempt %u/%u): %s",
                 attempt, kMaxAttempts, esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera bring-up failed after %u attempt(s)",
                 report.init_attempts);
        cameraManagerShutdown();
        return err;
    }
    s_initialised.store(true);

    // --- sensor PID (SPEC 4.1.1 rule 4) ------------------------------------
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor == nullptr) {
        ESP_LOGE(TAG, "esp_camera_sensor_get() returned NULL after a successful init");
        cameraManagerShutdown();
        return ESP_ERR_INVALID_STATE;
    }
    report.sensor_pid = sensor->id.PID;
    if (sensor->id.PID != OV2640_PID) {
        ESP_LOGE(TAG, "sensor PID 0x%04x is not OV2640 (0x%04x) — refusing to continue",
                 report.sensor_pid, static_cast<unsigned>(OV2640_PID));
        cameraManagerShutdown();
        return ESP_ERR_CAMERA_NOT_SUPPORTED;
    }
    report.pid_ok = true;
    ESP_LOGI(TAG, "sensor PID 0x%04x = OV2640", report.sensor_pid);

    // --- warm-up (SPEC 4.1.1 rule 6) ---------------------------------------
    // BOOTSTRAP frame access (camera_manager.h): the warm-up discard cannot
    // wait for frame/ — that module does not exist yet when the camera is
    // being proved. Frames are returned immediately, nothing is held.
    for (unsigned i = 0; i < kWarmupFrames; ++i) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb == nullptr) break;
        esp_camera_fb_return(fb);
        report.warmup_discarded++;
    }
    ESP_LOGI(TAG, "discarded %u warm-up frame(s)", report.warmup_discarded);

    // --- one JPEG (SPEC 4.1.1 rules 7, 8) ----------------------------------
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
        ESP_LOGE(TAG, "esp_camera_fb_get() returned NULL after warm-up");
        cameraManagerShutdown();
        return ESP_ERR_INVALID_STATE;
    }

    const size_t capacity = cameraManagerJpegCapacity();
    report.jpeg_width  = fb->width;
    report.jpeg_height = fb->height;

    if (fb->len == 0 || fb->len > capacity) {
        report.malformed_frames++;
        ESP_LOGE(TAG, "frame rejected (rule 7): len=%u capacity=%u (%ux%u) — "
                      "frame buffer is mis-sized",
                 static_cast<unsigned>(fb->len), static_cast<unsigned>(capacity),
                 report.jpeg_width, report.jpeg_height);
        esp_camera_fb_return(fb);
        cameraManagerShutdown();
        return ESP_ERR_INVALID_SIZE;
    }

    report.jpeg_bytes = fb->len;
    esp_camera_fb_return(fb);
    ESP_LOGI(TAG, "JPEG grabbed: %u bytes (%ux%u), capacity %u",
             static_cast<unsigned>(report.jpeg_bytes),
             report.jpeg_width, report.jpeg_height,
             static_cast<unsigned>(capacity));

    // --- runtime default back to VGA (SPEC 4.1.1 rule 1) -------------------
    // Failure here is not fatal: the camera keeps working at the init size.
    if (esp_err_t serr = cameraManagerSetFramesize(kDefaultFramesize);
        serr != ESP_OK) {
        ESP_LOGW(TAG, "default frame size not applied (%s) — staying at %d",
                 esp_err_to_name(serr), static_cast<int>(report.init_framesize));
        s_framesize.store(report.init_framesize);
    }

    return ESP_OK;
}

esp_err_t cameraManagerSensorReset() {
    sensor_t* sensor = sensorOrNull();
    if (sensor == nullptr || sensor->reset == nullptr) {
        ESP_LOGW(TAG, "sensor reset unavailable — camera not initialised");
        return ESP_ERR_INVALID_STATE;
    }
    const int rc = sensor->reset(sensor);   // OV2640: register defaults
    if (rc != 0) {
        ESP_LOGE(TAG, "sensor reset failed (%d)", rc);
        return ESP_FAIL;
    }
    // reset() restores the sensor's defaults, so the live settings must be
    // re-applied or a recovery would silently change the user's frame size.
    const framesize_t fs = s_framesize.load();
    if (sensor->set_framesize(sensor, fs) != 0) {
        ESP_LOGW(TAG, "re-apply frame size %d after reset failed",
                 static_cast<int>(fs));
    }
    sensor->set_quality(sensor, s_quality.load());
    ESP_LOGI(TAG, "sensor reset (frame size %d re-applied)",
             static_cast<int>(fs));
    return ESP_OK;
}

esp_err_t cameraManagerPowerCycle() {
    powerCyclePwdn();
    ESP_LOGI(TAG, "camera power-cycled through PWDN (GPIO%d)",
             BOARD_CAM_PWDN);
    return ESP_OK;
}

esp_err_t cameraManagerReinit(CameraReport& report,
                              CameraInitTestMode mode) {
    // The deepest rung of the 1.1.4 ladder: full driver shutdown, hardware
    // power-cycle, then the whole SPEC 4.1.1 bring-up again (including its
    // own internal 3-attempt retry and warm-up). Safe to call when the
    // camera is already down (cam_deinit guards its object).
    cameraManagerShutdown();
    powerCyclePwdn();
    return cameraManagerInit(report, mode);
}

bool cameraManagerGrabProbe(size_t* bytes) {
    if (bytes != nullptr) *bytes = 0;
    if (!s_initialised.load()) return false;
    // TEMPORARY smoke probe — see camera_manager.h: task 1.1.5 removes this
    // and frame/ becomes the only frame consumer (AGENTS rule 9).
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) return false;
    if (bytes != nullptr) *bytes = fb->len;
    const bool ok = fb->len > 0;
    esp_camera_fb_return(fb);
    return ok;
}

}  // namespace visioncam