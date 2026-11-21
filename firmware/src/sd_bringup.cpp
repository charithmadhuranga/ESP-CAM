// sd_bringup.cpp — see sd_bringup.h.
//
// Every ESP-IDF SDMMC detail below was checked against the pinned IDF 6.1.0
// source, not taken from SPEC's example (which marks itself "verify against
// the pinned ESP-IDF"). See docs/REFERENCE_ANALYSIS.md for the two places the
// verified behaviour differs from SPEC's snippet.

#include "sd_bringup.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"  // FATFS / f_getfree — volume geometry report below
#include "soc/sdmmc_pins.h"

#include "board_profile.h"

namespace visioncam {
namespace {

const char* TAG = "vc_sd";

sdmmc_card_t* s_card = nullptr;
std::atomic<bool> s_gpio4_ready{false};
std::atomic<bool> s_flash_led_enabled{false};

// --- Verified facts about ESP32 SDMMC in IDF 6.1.0 -------------------------
//
// 1. ESP32 uses SOC_SDMMC_USE_IOMUX (not SOC_SDMMC_USE_GPIO_MATRIX), so
//    s_check_pin_not_set() always returns true (sd_host_sdmmc.c:1216) and the
//    driver sets use_gpio_matrix = false. CONSEQUENCE: the clk/cmd/d0 fields
//    of sdmmc_slot_config_t are IGNORED on ESP32 — the pins are fixed per
//    slot. Setting them would be harmless but would imply control we do not
//    have, so they are not set.
//
// 2. Slot 1's fixed pins (components/esp_hal_sd/esp32/include/soc/
//    sdmmc_pins.h:21-26) are CLK 14, CMD 15, D0 2, D1 4, D2 12, D3 13 —
//    exactly the AI-Thinker SD pins. Slot 0 is occupied by the SPI flash
//    (sd_host_sdmmc.c:1231), so being on slot 1 is not optional.
//
// 3. SDMMC_HOST_DEFAULT() already resolves to SDMMC_HOST_SLOT_1 on ESP32
//    (sdmmc_default_configs.h:65), but it is set explicitly below so a future
//    IDF default change cannot silently move the bus onto the flash pins.
//
// 4. The bus width comes from slot.width. sdmmc_fix_host_flags()
//    (sdmmc_common.c:365-377) reads the slot width and masks host.flags down
//    to SDMMC_HOST_FLAG_1BIT when it is 1, then sdmmc_init_host_bus_width()
//    (sdmmc_common.c:222) never requests 4-bit. So slot.width = 1 alone is
//    what actually forces 1-bit mode.
static_assert(BOARD_SD_CLK == SDMMC_SLOT1_IOMUX_PIN_NUM_CLK,
              "board_profile.h SD CLK must match ESP32 SDMMC slot 1 (GPIO14); "
              "on ESP32 the SDMMC pins are fixed per slot, so this value is "
              "documentation only and a mismatch means the pin map is wrong");
static_assert(BOARD_SD_CMD == SDMMC_SLOT1_IOMUX_PIN_NUM_CMD,
              "board_profile.h SD CMD must match ESP32 SDMMC slot 1 (GPIO15)");
static_assert(BOARD_SD_D0 == SDMMC_SLOT1_IOMUX_PIN_NUM_D0,
              "board_profile.h SD D0 must match ESP32 SDMMC slot 1 (GPIO2)");
static_assert(BOARD_SD_D1 == SDMMC_SLOT1_IOMUX_PIN_NUM_D1,
              "board_profile.h SD D1 must match ESP32 SDMMC slot 1 (GPIO4)");
static_assert(BOARD_SD_D2 == SDMMC_SLOT1_IOMUX_PIN_NUM_D2,
              "board_profile.h SD D2 must match ESP32 SDMMC slot 1 (GPIO12)");
static_assert(BOARD_SD_D3 == SDMMC_SLOT1_IOMUX_PIN_NUM_D3,
              "board_profile.h SD D3 must match ESP32 SDMMC slot 1 (GPIO13)");

}  // namespace

const sdmmc_card_t* sdCard() { return s_card; }

esp_err_t sdFlashLedSet(bool enabled) {
    if (s_card == nullptr || !s_gpio4_ready.load()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err =
        gpio_set_level(static_cast<gpio_num_t>(BOARD_FLASH_LED), enabled ? 1 : 0);
    if (err == ESP_OK) s_flash_led_enabled.store(enabled);
    return err;
}

bool sdFlashLedEnabled() {
    return s_gpio4_ready.load() && s_flash_led_enabled.load();
}

esp_err_t sdMount(SdReport& report) {
    report = SdReport{};
    s_gpio4_ready.store(false);
    s_flash_led_enabled.store(false);

    // --- host ---------------------------------------------------------------
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;  // see fact 2 above

    // Deliberately NOT `host.flags = SDMMC_HOST_FLAG_1BIT` as SPEC 2.3's
    // example shows. Verified: (a) it is redundant — slot.width below already
    // forces 1-bit via sdmmc_fix_host_flags(); (b) it would also clear
    // SDMMC_HOST_FLAG_DEINIT_ARG, so unmount would call host.deinit() (tears
    // down the whole SDMMC peripheral) instead of deinit_p(slot)
    // (vfs_fat_sdmmc.c:447). Keeping the defaults is the correct behaviour.
    // Recorded as a divergence from SPEC in docs/REFERENCE_ANALYSIS.md.

    // --- slot ---------------------------------------------------------------
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;  // AGENTS rule 7 / SPEC 2.3: 1-bit is the supported config

    // --- mount config -------------------------------------------------------
    esp_vfs_fat_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;  // SPEC 2.3: never auto-format
    mount_cfg.max_files = 5;
    mount_cfg.allocation_unit_size = 16 * 1024;

    ESP_LOGI(TAG, "mounting SD: slot %d, %u-bit, freq default %d kHz",
             host.slot, static_cast<unsigned>(slot.width), host.max_freq_khz);

    const esp_err_t err =
        esp_vfs_fat_sdmmc_mount(kSdMountPoint, &host, &slot, &mount_cfg, &s_card);

    if (err != ESP_OK || s_card == nullptr) {
        s_card = nullptr;
        // Readable, non-fatal: storage features stay disabled and everything
        // else keeps running (SPEC 2.3, AGENTS rule 30).
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "SD mount failed: card not recognised or not formatted "
                          "(%s). NOT formatting — mount a card you are willing "
                          "to format by hand",
                     esp_err_to_name(err));
        } else {
            ESP_LOGE(TAG, "SD mount failed: %s. Check the card, the 1-bit wiring "
                          "and the 5 V supply; storage features stay disabled",
                     esp_err_to_name(err));
        }
        return err;
    }

    report.mounted = true;
    report.max_freq_khz = s_card->max_freq_khz;
    report.bus_width = 1;

    // Prints CID/CSD/OCR — this is where the exact card model comes from (D5).
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD mounted at %s, negotiated %u kHz",
             kSdMountPoint, static_cast<unsigned>(s_card->max_freq_khz));

    // --- FAT volume geometry -------------------------------------------------
    // Two independent views of one card: the CSD (physical capacity, printed
    // above) and the FAT BPB (the volume actually usable by every filesystem
    // user). They disagree on the owner's card — CSD 7.5 GB vs a 5.0 MB FAT
    // (docs/HARDWARE.md 5.2, found during the D2 R1 baseline run). Printing
    // both here, from IDF's own FATFS, says which layer the 5 MB lives in:
    // if IDF and arduino-esp32 agree, the number comes from the card's BPB,
    // not from either wrapper.
    FATFS* fatfs = nullptr;
    DWORD free_clusters = 0;
    const FRESULT fr = f_getfree("0:", &free_clusters, &fatfs);
    if (fr == FR_OK && fatfs != nullptr) {
        const uint64_t cluster_bytes =
            static_cast<uint64_t>(fatfs->csize) * fatfs->ssize;
        const uint64_t total_bytes = cluster_bytes * (fatfs->n_fatent - 2);
        const uint64_t free_bytes = cluster_bytes * free_clusters;
        const uint64_t csd_bytes =
            static_cast<uint64_t>(s_card->csd.capacity) * s_card->csd.sector_size;
        ESP_LOGI(TAG,
                 "FAT volume: %llu B usable, %llu B free, cluster %u B x %u "
                 "clusters — CSD capacity %llu B",
                 static_cast<unsigned long long>(total_bytes),
                 static_cast<unsigned long long>(free_bytes),
                 static_cast<unsigned>(cluster_bytes),
                 static_cast<unsigned>(fatfs->n_fatent - 2),
                 static_cast<unsigned long long>(csd_bytes));
        if (total_bytes * 8 < csd_bytes) {
            ESP_LOGW(TAG, "FAT volume is < 1/8 of the CSD capacity: the card's "
                          "partition/FAT does not cover the card "
                          "(docs/HARDWARE.md 5.2)");
        }
    } else {
        // FRESULT has no strerror() in IDF's FATFS — print the raw code.
        ESP_LOGW(TAG, "f_getfree(\"0:\") failed with FRESULT %d", static_cast<int>(fr));
    }

    // --- GPIO4 / flash lamp (task 0.3 step 13) ------------------------------
    // Keep GPIO4 low at startup; only the 1-bit SD configuration leaves
    // SD D1 available for the flash LED.
    gpio_config_t led = {};
    led.pin_bit_mask = 1ULL << BOARD_FLASH_LED;
    led.mode         = GPIO_MODE_OUTPUT;
    led.pull_up_en   = GPIO_PULLUP_DISABLE;
    led.pull_down_en = GPIO_PULLDOWN_DISABLE;
    esp_err_t led_err = gpio_config(&led);
    if (led_err == ESP_OK) {
        led_err = gpio_set_level(static_cast<gpio_num_t>(BOARD_FLASH_LED), 0);
    }
    if (led_err == ESP_OK) {
        s_gpio4_ready.store(true);
        report.gpio4_driven_low = true;
        ESP_LOGI(TAG, "GPIO%d (flash lamp / SD D1) driven low after mount",
                 BOARD_FLASH_LED);
    } else {
        ESP_LOGE(TAG, "GPIO%d flash LED initialization failed: %s",
                 BOARD_FLASH_LED, esp_err_to_name(led_err));
    }

    return ESP_OK;
}

esp_err_t sdSelfTest() {
    if (s_card == nullptr) return ESP_ERR_INVALID_STATE;

    const char* path = "/sdcard/vc_smoke.txt";
    static const char kPayload[] = "visioncam phase0 sd 1-bit ok";

    FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        ESP_LOGE(TAG, "self-test: fopen(w) failed for %s", path);
        return ESP_ERR_NO_MEM;
    }
    const size_t written = std::fwrite(kPayload, 1, sizeof(kPayload), f);
    std::fclose(f);
    if (written != sizeof(kPayload)) {
        ESP_LOGE(TAG, "self-test: wrote %u of %u bytes",
                 static_cast<unsigned>(written),
                 static_cast<unsigned>(sizeof(kPayload)));
        return ESP_FAIL;
    }

    f = std::fopen(path, "r");
    if (f == nullptr) {
        ESP_LOGE(TAG, "self-test: fopen(r) failed for %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    char buf[64] = {};
    const size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);

    if (got != sizeof(kPayload) || std::memcmp(buf, kPayload, sizeof(kPayload)) != 0) {
        ESP_LOGE(TAG, "self-test: read back %u bytes, mismatch", static_cast<unsigned>(got));
        return ESP_ERR_INVALID_CRC;
    }
    if (std::remove(path) != 0) {
        ESP_LOGE(TAG, "self-test: could not delete %s", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SD self-test OK: wrote, read back and deleted %s", path);
    return ESP_OK;
}

void sdUnmount() {
    if (s_card == nullptr) return;
    if (sdFlashLedEnabled()) {
        const esp_err_t led_err = sdFlashLedSet(false);
        if (led_err != ESP_OK) {
            ESP_LOGE(TAG, "GPIO%d flash LED could not be turned off: %s",
                     BOARD_FLASH_LED, esp_err_to_name(led_err));
        }
    }
    // IDF 6.1 naming inconsistency, verified in esp_vfs_fat.h: the mount is
    // esp_vfs_fat_sdmmc_mount() (line 190) but the unmount is
    // esp_vfs_fat_sdcard_unmount() (line 249) — there is no
    // esp_vfs_fat_sdmmc_unmount().
    esp_vfs_fat_sdcard_unmount(kSdMountPoint, s_card);
    s_card = nullptr;
    s_gpio4_ready.store(false);
    s_flash_led_enabled.store(false);
    ESP_LOGI(TAG, "SD unmounted");
}

}  // namespace visioncam
