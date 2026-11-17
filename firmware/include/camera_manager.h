// camera_manager.h — the camera manager (plan 02 task 1.1.3; SPEC section 4.1.1).
//
// Owns the camera DRIVER LIFECYCLE: bring-up sequence, sensor identity,
// frame size / quality settings, and the shutdown/re-init primitives the
// NULL-frame recovery ladder (plan 02 task 1.1.4, lives in frame/) drives.
//
// Frame buffers are NOT owned here (AGENTS rule 9): from 1.1.5 on, only
// the frame manager acquires and returns camera frame buffers. The single
// exception in this file is BOOTSTRAP frame access — the SPEC 4.1.1
// warm-up discard (rule 6) and the validation read (rule 7) must run
// before any consumer exists, because the camera has to be proven working
// before anything can depend on it. Both are init-only, documented at
// their call sites, and cameraManagerGrabProbe() below is the temporary
// Phase 0 smoke probe that task 1.1.5 removes (its comment says so).
//
// Thread-safety: settings changes (setFramesize/setQuality) are called
// from the config path (task 1.2) while frame/ acquires frames. They go
// through the sensor's own driver calls, which serialise internally; the
// recovery primitives are called only by frame/'s capture task.

#pragma once

#include <cstddef>

#include "esp_err.h"
#include "esp_camera.h"
#include "sensor.h"

namespace visioncam {

// Test-only init sabotage (never used by production paths). Phase 0
// verified the mechanism on hardware: removing XCLK makes esp_camera_init
// genuinely fail (SCCB cannot reach the sensor), unlike holding PWDN which
// R2's camera_probe releases itself (esp_camera.c:204-216).
enum class CameraInitTestMode : uint8_t {
    None,               // production bring-up
    ForceFirstFailure,  // attempt 1 fails, attempts 2..3 must recover
                        // (Phase 0 step 11's forced-failure test)
    ForceAllAttempts,   // every attempt fails (task 1.1.4's ladder test:
                        // it needs a deterministically dead camera)
};

// What the bring-up observed. Filled in even on failure so the serial log
// can say exactly how far it got instead of only "init failed".
struct CameraReport {
    bool     initialised = false;
    bool     pid_ok = false;
    unsigned sensor_pid = 0;          // OV2640_PID is 0x26 — sensor.h:21
    unsigned init_attempts = 0;       // 1..3 (initial + two retries)
    unsigned warmup_discarded = 0;    // SPEC 4.1.1 rule 6 wants >= 3
    unsigned malformed_frames = 0;    // rule 7: len == 0 or len > capacity
    size_t   jpeg_bytes = 0;
    unsigned jpeg_width = 0;
    unsigned jpeg_height = 0;
    framesize_t init_framesize = FRAMESIZE_VGA;
};

// Runs the SPEC 4.1.1 sequence:
//   1. largest frame size the detected PSRAM holds (about 2 MB -> SVGA,
//      about 4 MB -> UXGA), with an explicit jpeg_buffer_size for sizes
//      the default width*height/5 would under-provision (rule 8)
//   2. fb_count = 2, frame buffers in PSRAM, CAMERA_GRAB_LATEST, 20 MHz XCLK
//   3. up to three init attempts, power-cycling PWDN (GPIO32) between them
//   4. sensor PID must be OV2640, otherwise fail with a readable message
//   5. discard >= 3 warm-up frames so auto exposure / white balance settle
//   6. grab and validate one JPEG against the rule-7 capacity
//   7. drop back to the VGA runtime default (rule 1)
//
// CameraInitTestMode (Phase 0 step 11 and task 1.1.4's ladder test):
// ForceFirstFailure makes attempt 1 genuinely fail by removing the sensor
// clock (pin_xclk = -1) — NOT by holding PWDN, which R2's camera_probe
// releases itself (esp_camera.c:204-216, hardware-verified 2026-11-15) —
// then the normal retry power-cycles PWDN and must recover. A real failure
// on a real recovery path, not a simulated one. ForceAllAttempts keeps the
// clock off for every attempt, so the bring-up deterministically fails:
// that is what the NULL-frame recovery-ladder test needs.
//
// Returns ESP_OK on success. On failure the camera is left shut down.
esp_err_t cameraManagerInit(CameraReport& report,
                            CameraInitTestMode mode = CameraInitTestMode::None);

// Releases the driver. Safe to call when the camera was never initialised
// (esp_camera_deinit() guards its state, esp_camera.c:383-389).
void cameraManagerShutdown();

// True while the driver holds an initialised camera.
bool cameraManagerInitialised();

// Live frame-size / quality settings (SPEC 4.1.1 rule 1: the user's frame
// size is applied at runtime, not by rebooting). No-ops with a log when
// the camera is not initialised. Both are safe to call while frame/ is
// streaming — the sensor driver applies them on the next captured frame.
esp_err_t cameraManagerSetFramesize(framesize_t framesize);
esp_err_t cameraManagerSetQuality(int quality);   // 0..63, lower = better

// Current settings, for /status and the UI. Returns the driver defaults
// (FRAMESIZE_VGA, quality 12) when not initialised.
framesize_t cameraManagerFramesize();
int cameraManagerQuality();

// The rule-7 capacity the frame manager validates every frame against:
// the configured jpeg_buffer_size if one was set at init, else the R2
// default width*height/5 of the CURRENT frame size (cam_hal.c:619).
// 0 when the camera is not initialised.
size_t cameraManagerJpegCapacity();

// Human-readable sensor name from the driver's own table ("OV2640").
// Never returns nullptr.
const char* cameraManagerSensorName();

// --- primitives for the frame manager's recovery ladder (task 1.1.4) ----
// Rung 1: sensor software reset + re-apply the current frame size/quality.
// Cheap, no camera de-init; does not disturb the driver's buffers.
esp_err_t cameraManagerSensorReset();

// Rung 2: hardware power-cycle through PWDN (GPIO32, active high) WITHOUT
// touching the driver — pair it with a rung-3 re-init for a full restart.
esp_err_t cameraManagerPowerCycle();

// Rung 3: shutdown + PWDN power-cycle + the full SPEC 4.1.1 bring-up.
// The deepest recovery before giving up and raising CameraFailed.
// force_first_failure (test-only): sabotages the first init attempt by
// removing XCLK, so a ladder test can make every rung deterministically
// fail — the same verified mechanism as the Phase 0 step-11 forced failure.
// Never set it in production paths.
esp_err_t cameraManagerReinit(CameraReport& report,
                              CameraInitTestMode mode =
                                  CameraInitTestMode::None);

// TEMPORARY (task 1.1.3): the Phase 0 step-14 smoke probe — one fb_get +
// fb_return proving the camera still produces frames while Wi-Fi and SD
// are active. The ONLY steady-state frame access outside frame/, and it is
// deleted when the frame manager lands (task 1.1.5): the probe moves there
// behind the lease API and AGENTS rule 9 becomes literally true.
bool cameraManagerGrabProbe(size_t* bytes);

// TEST ONLY (task 1.1.4 smoke): while armed, every esp_camera_init attempt
// runs with XCLK removed, so bring-up deterministically fails — the same
// verified mechanism as CameraInitTestMode, but STICKY, because the NULL-
// frame recovery ladder calls re-init internally and its test needs every
// rung to fail. Production never arms this; the smoke test disarms it at
// the end of the ladder test.
void cameraManagerSetTestSabotage(bool armed);
bool cameraManagerTestSabotageArmed();

}  // namespace visioncam