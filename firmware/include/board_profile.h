// board_profile.h — AI-Thinker ESP32-CAM pin map (the ONLY place pin
// constants may live; AGENTS rule 6).
//
// Verified 2026-11-15 against three independent sources, all in agreement:
//   1. SPEC.md section 2.2 (camera pins)
//   2. R1 reference/ESP32-CAM_MJPEG2SD-master/src/camera_pins.h:187-206
//      (behavioural evidence only — not copied, values cross-checked)
//   3. Owner-supplied board pin table (2026-11-15), which additionally
//      supplies the microSD pins that SPEC section 2.2 does not tabulate.
//
// Do not duplicate these values anywhere else in the codebase.

#pragma once

// ---------------------------------------------------------------------------
// Camera (OV2640) — these GPIOs are wired to the camera and must not be
// reused for other peripherals.
// ---------------------------------------------------------------------------
#define BOARD_CAM_PWDN      32
#define BOARD_CAM_RESET     -1   // no reset line; software reset only
#define BOARD_CAM_XCLK       0   // strapping pin: must be HIGH after reset
#define BOARD_CAM_SIOD      26   // SCCB data
#define BOARD_CAM_SIOC      27   // SCCB clock

#define BOARD_CAM_Y9        35   // D7  (input-only)
#define BOARD_CAM_Y8        34   // D6  (input-only)
#define BOARD_CAM_Y7        39   // D5  (input-only)
#define BOARD_CAM_Y6        36   // D4  (input-only)
#define BOARD_CAM_Y5        21   // D3
#define BOARD_CAM_Y4        19   // D2
#define BOARD_CAM_Y3        18   // D1
#define BOARD_CAM_Y2         5   // D0

#define BOARD_CAM_VSYNC     25
#define BOARD_CAM_HREF      23
#define BOARD_CAM_PCLK      22

// ---------------------------------------------------------------------------
// microSD (SDMMC host)
//
// VisionCam's initial supported configuration is 1-bit mode: only CLK, CMD
// and D0 are used, which frees GPIO4 (D1) and GPIO12 (D2) — SPEC section 2.3
// and line 333. 4-bit mode would put D1 back on GPIO4, colliding with the
// flash LED, so 1-bit is the supported default, not an optimisation.
//
// Boot-strapping pins on this bus: GPIO2 (D0), GPIO12 (D2), GPIO15 (CMD).
// They must not be driven to their strap levels at reset or the module will
// not boot.
// ---------------------------------------------------------------------------
#define BOARD_SD_CLK        14
#define BOARD_SD_CMD        15
#define BOARD_SD_D0          2   // strapping pin
#define BOARD_SD_D1          4   // ALSO the flash LED (1-bit mode leaves it free)
#define BOARD_SD_D2         12   // strapping pin — also sets flash voltage (MTDI)
#define BOARD_SD_D3         13   // unused in 1-bit mode

// ---------------------------------------------------------------------------
// LEDs and other board functions
// ---------------------------------------------------------------------------
#define BOARD_FLASH_LED      4   // shared with SD D1; active HIGH
#define BOARD_STATUS_LED    33   // red signal LED (R1 lists GPIO33)

// GPIO16 is reserved for PSRAM on this module — never use it.
// GPIO1 (U0T) and GPIO3 (U0R) are the UART programming/debug pins.

// ---------------------------------------------------------------------------
// Expected hardware (verified on the board in Phase 0, not assumed)
// ---------------------------------------------------------------------------
#define BOARD_FLASH_SIZE_MB   4   // esp32cam board definition; confirm on hardware
// PSRAM size is reported at boot by the smoke test; do not hardcode it here.
