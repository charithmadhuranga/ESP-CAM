// camera_bringup.h — SPEC section 4.1.1 camera initialisation.
//
// Kept in its own module so it compiles, and later can be exercised,
// independently of storage and networking (AGENTS rule 25).

#pragma once

#include <cstddef>

#include "esp_err.h"
#include "esp_camera.h"

namespace visioncam {

// What the bring-up observed. Filled in even on failure so the serial log can
// say exactly how far it got instead of only "init failed".
struct CameraReport {
    bool initialised = false;
    bool pid_ok = false;
    unsigned sensor_pid = 0;        // OV2640_PID is 0x26 — sensor.h:21
    unsigned init_attempts = 0;     // 1..3 (initial + two retries)
    unsigned warmup_discarded = 0;  // SPEC 4.1.1 rule 6 wants >= 3
    unsigned malformed_frames = 0;  // rule 7: len == 0 or len > capacity
    size_t jpeg_bytes = 0;
    unsigned jpeg_width = 0;
    unsigned jpeg_height = 0;
    framesize_t init_framesize = FRAMESIZE_VGA;
};

// Runs the SPEC 4.1.1 sequence:
//   1. largest frame size the detected PSRAM holds (about 2 MB -> SVGA,
//      about 4 MB -> UXGA)
//   2. fb_count = 2, frame buffers in PSRAM, CAMERA_GRAB_LATEST, 20 MHz XCLK
//   3. up to three init attempts, power-cycling PWDN (GPIO32) between them
//   4. sensor PID must be OV2640, otherwise fail with a readable message
//   5. discard >= 3 warm-up frames so auto exposure / white balance settle
//   6. grab one JPEG, drop and count it if it breaks rule 7, else measure it
//   7. drop back to the VGA runtime default (rule 1)
//
// force_first_failure (Phase 0 step 11): holds PWDN asserted during the
// FIRST attempt only, so the sensor is genuinely unreachable and
// esp_camera_init really fails — then the normal retry power-cycles PWDN and
// must recover. A real failure on a real recovery path, not a simulated one.
//
// Returns ESP_OK on success. On failure the camera is left shut down.
esp_err_t cameraBringup(CameraReport& report, bool force_first_failure = false);

// Releases the driver. Safe to call when the camera was never initialised.
void cameraShutdown();

// Grabs exactly one frame from the already-initialised camera and returns it
// immediately — the "camera still running" half of the step-14 Wi-Fi cycle.
// Frame access stays inside this module (AGENTS rule 9). Returns false when
// no frame could be produced; *bytes (optional) receives the frame length.
bool cameraGrabProbe(size_t* bytes);

}  // namespace visioncam
