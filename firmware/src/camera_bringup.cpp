// camera_bringup.cpp — see camera_bringup.h.

#include "camera_bringup.h"

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

// SPEC 4.1.1 rule 1: initialise at the largest frame size the PSRAM holds.
framesize_t initFrameSizeFor(size_t psram_bytes) {
    if (psram_bytes >= 3u * 1024u * 1024u) return FRAMESIZE_UXGA;  // ~4 MB
    if (psram_bytes >= 1500u * 1024u)      return FRAMESIZE_SVGA;  // ~2 MB
    return FRAMESIZE_VGA;
}

// SPEC 4.1.1 rules 2, 3, 8: 2 buffers in PSRAM, GRAB_LATEST, 20 MHz XCLK,
// jpeg_buffer_size left at 0 so R2 uses its default width*height/5
// (cam_hal.c:619) — that is the capacity rule 7 is checked against.
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
    cfg.jpeg_quality  = 12;               // 0..63, lower = better
    cfg.fb_count      = 2;
    cfg.fb_location   = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode     = CAMERA_GRAB_LATEST;
    cfg.jpeg_buffer_size = 0;             // default width*height/5
    cfg.sccb_i2c_port = -1;
}

// SPEC 4.1.1 rule 5: power-cycle the sensor through PWDN between attempts.
// PWDN (GPIO32) is active high: assert = powered down, release = powered up.
void powerCyclePwdn() {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOARD_CAM_PWDN;
    io.mode         = GPIO_MODE_OUTPUT;
    io.pull_up_en   = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io);

    gpio_set_level(static_cast<gpio_num_t>(BOARD_CAM_PWDN), 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(static_cast<gpio_num_t>(BOARD_CAM_PWDN), 0);
    vTaskDelay(pdMS_TO_TICKS(20));
}

// Leaves the sensor powered down (PWDN asserted) so the next esp_camera_init
// genuinely cannot reach it — the forced first-attempt failure of step 11.
// NOTE (2026-11-15, hardware): this does NOT work on its own — R2's
// camera_probe releases PWDN itself at the start of every init
// (esp_camera.c:204-216), so attempt 1 still succeeded while we held it.
// cameraBringup therefore forces the failure via pin_xclk instead.
void holdPwdnAsserted() {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOARD_CAM_PWDN;
    io.mode         = GPIO_MODE_OUTPUT;
    io.pull_up_en   = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io);

    gpio_set_level(static_cast<gpio_num_t>(BOARD_CAM_PWDN), 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

}  // namespace

void cameraShutdown() {
    // esp_camera_init()'s doc comment still claims the module "can only be
    // called once and there is no way to de-initialize" (esp_camera.h:187-188).
    // That is stale in esp32-camera 2.1.9: esp_camera_deinit() is declared
    // (esp_camera.h:203), implemented (esp_camera.c:379) and nulls s_state
    // (esp_camera.c:387).
    //
    // Why every exit path calls it: a *failed* esp_camera_init already cleans
    // up after itself via its `fail:` label (esp_camera.c:374-376), but a
    // *successful* one leaves s_state set — and the PID check, warm-up and
    // malformed-frame exits below all run after a successful init. Without an
    // explicit deinit a later re-init returns ESP_ERR_INVALID_STATE from
    // camera_probe() (esp_camera.c:177-178). Recorded in
    // docs/REFERENCE_ANALYSIS.md.
    esp_camera_deinit();
}

esp_err_t cameraBringup(CameraReport& report, bool force_first_failure) {
    report = CameraReport{};

    const size_t psram = esp_psram_get_size();
    report.init_framesize = initFrameSizeFor(psram);
    ESP_LOGI(TAG, "PSRAM %u B -> init frame size %d",
             static_cast<unsigned>(psram), static_cast<int>(report.init_framesize));

    camera_config_t cfg{};
    fillConfig(cfg, report.init_framesize);

    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        report.init_attempts = attempt;
        if (attempt > 1) {
            cameraShutdown();
            powerCyclePwdn();
            cfg.pin_xclk = BOARD_CAM_XCLK;  // undo the forced-failure sabotage
        } else if (force_first_failure) {
            // Step 11: make attempt 1 genuinely fail by removing the sensor's
            // clock (pin_xclk = -1 -> GPIO0 never driven -> the SCCB probe
            // cannot see the sensor). Holding PWDN instead does NOT work: R2's
            // camera_probe releases PWDN itself at the start of every init
            // (esp_camera.c:204-216 — verified on hardware 2026-11-15).
            cfg.pin_xclk = -1;
            ESP_LOGW(TAG, "forced failure test: XCLK removed — attempt 1 is "
                          "expected to fail");
        }
        err = esp_camera_init(&cfg);
        if (err == ESP_OK) break;
        ESP_LOGE(TAG, "esp_camera_init failed (attempt %u/%u): %s",
                 attempt, kMaxAttempts, esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera bring-up failed after %u attempt(s)", report.init_attempts);
        cameraShutdown();
        return err;
    }
    report.initialised = true;

    // --- sensor PID (SPEC 4.1.1 rule 4) ------------------------------------
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor == nullptr) {
        ESP_LOGE(TAG, "esp_camera_sensor_get() returned NULL after a successful init");
        cameraShutdown();
        return ESP_ERR_INVALID_STATE;
    }
    report.sensor_pid = sensor->id.PID;
    if (sensor->id.PID != OV2640_PID) {
        ESP_LOGE(TAG, "sensor PID 0x%04x is not OV2640 (0x%04x) — refusing to continue",
                 report.sensor_pid, static_cast<unsigned>(OV2640_PID));
        cameraShutdown();
        return ESP_ERR_CAMERA_NOT_SUPPORTED;
    }
    report.pid_ok = true;
    ESP_LOGI(TAG, "sensor PID 0x%04x = OV2640", report.sensor_pid);

    // --- warm-up (SPEC 4.1.1 rule 6) ---------------------------------------
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
        cameraShutdown();
        return ESP_ERR_INVALID_STATE;
    }

    const size_t capacity =
        cfg.jpeg_buffer_size > 0
            ? cfg.jpeg_buffer_size
            : (static_cast<size_t>(fb->width) * static_cast<size_t>(fb->height)) / 5u;

    report.jpeg_width  = fb->width;
    report.jpeg_height = fb->height;

    if (fb->len == 0 || fb->len > capacity) {
        report.malformed_frames++;
        ESP_LOGE(TAG, "frame rejected (rule 7): len=%u capacity=%u (%ux%u) — "
                      "frame buffer is mis-sized",
                 static_cast<unsigned>(fb->len), static_cast<unsigned>(capacity),
                 report.jpeg_width, report.jpeg_height);
        esp_camera_fb_return(fb);
        cameraShutdown();
        return ESP_ERR_INVALID_SIZE;
    }

    report.jpeg_bytes = fb->len;
    esp_camera_fb_return(fb);
    ESP_LOGI(TAG, "JPEG grabbed: %u bytes (%ux%u), capacity %u",
             static_cast<unsigned>(report.jpeg_bytes),
             report.jpeg_width, report.jpeg_height,
             static_cast<unsigned>(capacity));

    // --- runtime default back to VGA (SPEC 4.1.1 rule 1) -------------------
    if (sensor->set_framesize(sensor, FRAMESIZE_VGA) != 0) {
        ESP_LOGW(TAG, "set_framesize(VGA) failed — staying at the init size");
    } else {
        ESP_LOGI(TAG, "runtime frame size set to VGA");
    }

    return ESP_OK;
}

bool cameraGrabProbe(size_t* bytes) {
    if (bytes != nullptr) *bytes = 0;
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) return false;
    if (bytes != nullptr) *bytes = fb->len;
    const bool ok = fb->len > 0;
    esp_camera_fb_return(fb);
    return ok;
}

}  // namespace visioncam
