// sd_bringup.h — SPEC section 2.3: SD card over the ESP-IDF SDMMC host in
// 1-bit mode, mounted read/write, never auto-formatted.
//
// Ownership: from Phase 2 this module grows into the storage manager and
// becomes the ONLY place filesystem operations happen (AGENTS rule 10).
// The smoke-test file I/O already lives here for that reason.

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "sdmmc_cmd.h"

namespace visioncam {

inline constexpr const char* kSdMountPoint = "/sdcard";

struct SdReport {
    bool mounted = false;
    bool gpio4_driven_low = false;  // task 0.3 step 13 — UNVERIFIED on hardware
    bool self_test_ok = false;
    uint32_t max_freq_khz = 0;
    uint8_t bus_width = 0;          // expected 1
};

// Mounts at kSdMountPoint with format_if_mount_failed = false.
// On failure nothing is formatted, no configuration is lost, and storage
// features simply stay disabled while the rest of the firmware keeps running
// (SPEC section 2.3, AGENTS rule 30).
esp_err_t sdMount(SdReport& report);

// GPIO4 is the flash LED and SD D1. It is controllable only after the
// supported 1-bit SD mount has configured the pin as an output.
esp_err_t sdFlashLedSet(bool enabled);
bool sdFlashLedEnabled();

// write -> read back -> compare -> delete (task 0.3 step 12).
esp_err_t sdSelfTest();

void sdUnmount();

// The mounted card, or NULL. Never dereferenced outside this module.
const sdmmc_card_t* sdCard();

}  // namespace visioncam
