// tools/sd_format/src/main.cpp — one-shot SD card format tool (Phase 0).
//
// Why this exists
// ---------------
// The owner's 8 GB SanDisk card carries a bootable Linux Mint image
// (owner, 2026-11-16): a small FAT boot/EFI partition plus an ext4 root.
// FATFS can only see the FAT one, which is why VisionCam reported a
// 5,216,256 B volume next to an 8,053,063,680 B CSD (docs/HARDWARE.md 4.2).
// The owner asked for the card to be rewritten with a suitable file system
// and partition table. This tool does exactly that, once, behind a typed
// confirmation.
//
// VisionCam itself NEVER formats a card (SPEC 2.3: format_if_mount_failed
// stays false; a mount failure is reported, not "fixed"), so formatting
// lives here — a standalone utility flashed instead of the firmware, never
// part of a release build.
//
// Clean-room (owner decision D1): no R1 code. The layout is written by
// FatFs f_mkfs from the pinned ESP-IDF 6.1.0. With FF_FS_EXFAT=0 and
// FF_LBA64=0 in that ffconf.h (verified), FM_ANY on an 8 GB card produces
// an MBR (one partition, start LBA 63, type 0x0C FAT32X — N_SEC_TRACK=63
// and the system-ID selection both live in ff.c create_partition/f_mkfs)
// plus a FAT32 volume: 8 GB / 32 KB clusters is ~244k clusters, far above
// the 65525-cluster FAT16 ceiling, so f_mkfs must upgrade to FAT32.
// Result readable by VisionCam, R1 and any PC.
//
// Console protocol — 115200 baud, one ASCII line per command:
//     info     card identity (CID/CSD), MBR partition table, FAT geometry
//     format   destructive: prints a warning, then requires the exact line
//              "ERASE" (case-sensitive) before a single byte is written
//     help     usage
// Boot runs `info` automatically so a plain serial capture records the
// pre-format state with no input at all.
//
// Evidence from the owner's board: docs/evidence/phase0-sd-format.log

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ff.h"  // BYTE/FRESULT/... — must precede diskio_sdmmc.h, whose
                 // declarations are written in FatFs types
#include "diskio_sdmmc.h"  // ff_diskio_register_sdmmc
#include "driver/sdmmc_host.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdmmc_cmd.h"

namespace {

constexpr char kDriveStr[] = "0:";  // FatFs logical drive (VolToPart[0] =
                                    // {0,0} auto-detect, ff_diskio.c)
constexpr size_t kLineMax = 128;
constexpr char kEraseToken[] = "ERASE";
constexpr char kSelfTestPath[] = "0:/VCSDFMT.TXT";  // 8.3 name, no LFN needed

// --- state -------------------------------------------------------------------
// A single console task touches all of this; no locking needed.

sdmmc_card_t* s_card = nullptr;  // the host struct is embedded in the card
                                 // (sdmmc_cmd.c drives card->host.*), so no
                                 // separate host lifetime to manage
FATFS s_fs;        // includes a 512 B window — static, not stack
FIL s_fil;         // self-test file object — static
BYTE s_mkfsWork[4096];  // f_mkfs work buffer: static = internal SRAM =
                        // DMA-safe for the SDMMC controller
bool s_fsMounted = false;
bool s_awaitingErase = false;

// --- console input -----------------------------------------------------------
// fgets()/stdin returns EOF immediately on this IDF configuration (the simple
// VFS read reports "no data" as end-of-file), so the UART0 driver is used
// explicitly and lines are assembled one byte at a time — the pattern proven
// by tools/nvs_provision. If the console already installed the driver, read
// from it as-is.
bool consoleReady = false;

void consoleInit() {
    if (uart_is_driver_installed(UART_NUM_0)) {
        consoleReady = true;
        std::printf("sdformat: console driver already installed\n");
        return;
    }
    uart_config_t cfg = {};
    cfg.baud_rate  = 115200;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    if (uart_param_config(UART_NUM_0, &cfg) != ESP_OK) return;
    if (uart_driver_install(UART_NUM_0, 1024, 0, 0, nullptr, 0) != ESP_OK) {
        return;
    }
    consoleReady = true;
    std::printf("sdformat: console driver installed\n");
}

// Blocks until a line arrives. Returns false for an oversized line (its
// content is discarded up to the newline; buf[0] is then '\0').
bool readLine(char* buf, size_t max) {
    size_t i = 0;
    bool overflow = false;
    for (;;) {
        uint8_t c = 0;
        const int n = uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY);
        if (n != 1) continue;
        if (c == '\n') {
            if (overflow) {
                buf[0] = '\0';
                return false;
            }
            buf[i] = '\0';
            return true;
        }
        if (c == '\r') continue;
        if (overflow) continue;
        if (i + 1 < max) {
            buf[i++] = static_cast<char>(c);
        } else {
            overflow = true;
        }
    }
}

// --- card bring-up -----------------------------------------------------------

// Same sequence as esp_vfs_fat_sdmmc_sdcard_init() in the pinned IDF
// (host init -> slot init -> card probe), spelled out with public APIs so the
// tool does not depend on vfs_fat_internal.h.
//
// Verified on this IDF (docs/REFERENCE_ANALYSIS.md, "SDMMC mount" entry):
//   * ESP32 SDMMC pins are fixed per slot (IOMUX, no GPIO matrix): slot 1 =
//     CLK 14 / CMD 15 / D0 2 — the AI-Thinker SD map that lives in
//     firmware/include/board_profile.h. Slot 0 is the SPI flash, so slot 1 is
//     not a choice. The slot config's clk/cmd/d0 fields are inert on ESP32,
//     which is why no pin constant appears in this file (AGENTS rule 6).
//   * The bus width comes from slot.width alone; 1 bit is the only supported
//     VisionCam configuration (AGENTS rule 7), set exactly as the firmware
//     sets it.
//   * host.flags is left at its SDMMC_HOST_DEFAULT value on purpose — forcing
//     SDMMC_HOST_FLAG_1BIT would clear SDMMC_HOST_FLAG_DEINIT_ARG (see the
//     sd_bringup.cpp comment for the full reasoning).
bool cardInit() {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;

    esp_err_t err = (*host.init)();
    if (err != ESP_OK) {
        std::printf("sdformat: host init failed: %s\n", esp_err_to_name(err));
        return false;
    }
    if ((err = sdmmc_host_init_slot(host.slot, &slot)) != ESP_OK) {
        std::printf("sdformat: slot init failed: %s\n", esp_err_to_name(err));
        return false;
    }

    auto* card = static_cast<sdmmc_card_t*>(
        std::calloc(1, sizeof(sdmmc_card_t)));
    if (card == nullptr) {
        std::printf("sdformat: out of memory for sdmmc_card_t\n");
        return false;
    }
    if ((err = sdmmc_card_init(&host, card)) != ESP_OK) {
        std::printf("sdformat: card init failed: %s\n", esp_err_to_name(err));
        std::printf("sdformat: varying errors usually mean bad slot contact "
                    "— re-seat the card (docs/HARDWARE.md 5.1)\n");
        std::free(card);
        return false;
    }
    s_card = card;
    // Hand the card to FatFs's diskio layer as logical drive 0 ("0:").
    // VolToPart[0] = {0,0} (ff_diskio.c) makes mount/format auto-detect
    // whatever partition table is on the card.
    ff_diskio_register_sdmmc(0, card);
    std::printf("sdformat: card initialised (slot %d, %u-bit)\n",
                host.slot, static_cast<unsigned>(slot.width));
    return true;
}

// --- MBR partition table (raw sector 0, no filesystem involved) --------------

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

const char* partitionTypeName(uint8_t t) {
    switch (t) {
        case 0x01: return "FAT12";
        case 0x04: return "FAT16 <32MB";
        case 0x06: return "FAT16";
        case 0x07: return "NTFS/exFAT";
        case 0x0B: return "FAT32";
        case 0x0C: return "FAT32 (LBA)";
        case 0x0E: return "FAT16 (LBA)";
        case 0x82: return "Linux swap";
        case 0x83: return "Linux";
        case 0xEE: return "GPT protective";
        case 0xEF: return "EFI system";
        default:   return "unknown";
    }
}

void printMbr() {
    if (s_card == nullptr) return;
    uint8_t sec[512];
    const esp_err_t err = sdmmc_read_sectors(s_card, sec, 0, 1);
    if (err != ESP_OK) {
        std::printf("sdformat: sector 0 read failed: %s\n",
                    esp_err_to_name(err));
        return;
    }
    if (!(sec[510] == 0x55 && sec[511] == 0xAA)) {
        // The other layout f_mkfs can produce (with FM_SFD, which this tool
        // never uses) is a "superfloppy": a FAT boot sector at sector 0.
        if (std::memcmp(sec + 54, "FAT", 3) == 0 ||
            std::memcmp(sec + 82, "FAT", 3) == 0) {
            std::printf("sdformat: sector 0 is a FAT boot sector — "
                        "unpartitioned (superfloppy) volume\n");
        } else {
            std::printf("sdformat: sector 0 has no 55AA signature and no FAT "
                        "boot sector — no partition table\n");
        }
        return;
    }
    std::printf("sdformat: MBR signature 55AA present, entries:\n");
    bool any = false;
    bool gptProtective = false;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = sec + 446 + 16 * i;
        const uint8_t type = e[4];
        if (type == 0) continue;
        any = true;
        if (type == 0xEE) gptProtective = true;
        const uint32_t lba = le32(e + 8);
        const uint32_t cnt = le32(e + 12);
        std::printf("sdformat:   p%d: type 0x%02X (%s), start LBA %lu, "
                    "%lu sectors = %llu B\n",
                    i + 1, type, partitionTypeName(type),
                    static_cast<unsigned long>(lba),
                    static_cast<unsigned long>(cnt),
                    static_cast<unsigned long long>(cnt) * 512ULL);
    }
    if (!any) std::printf("sdformat:   (all four entries empty)\n");
    if (gptProtective) {
        std::printf("sdformat:   protective MBR — the disk is GPT; this "
                    "FatFs build scans MBR only (FF_LBA64=0)\n");
    }
}

// --- FAT geometry -------------------------------------------------------------

// Returns true when a FAT volume is mounted on the drive. FR_NO_FILESYSTEM
// is the expected pre-format state of a card with no FAT at all, and is
// reported as such instead of as an error.
bool fatMount() {
    if (s_fsMounted) return true;
    const FRESULT fr = f_mount(&s_fs, kDriveStr, 1);
    if (fr == FR_OK) {
        s_fsMounted = true;
        return true;
    }
    if (fr == FR_NO_FILESYSTEM) {
        std::printf("sdformat: no FAT filesystem on the card (FRESULT %d) "
                    "— run 'format'\n", static_cast<int>(fr));
    } else {
        std::printf("sdformat: f_mount failed, FRESULT %d\n",
                    static_cast<int>(fr));
    }
    return false;
}

void printFatGeometry() {
    DWORD freeClusters = 0;
    FATFS* fs = nullptr;
    const FRESULT fr = f_getfree(kDriveStr, &freeClusters, &fs);
    if (fr != FR_OK || fs == nullptr) {
        std::printf("sdformat: f_getfree failed, FRESULT %d\n",
                    static_cast<int>(fr));
        return;
    }
    const uint64_t clusterBytes =
        static_cast<uint64_t>(fs->csize) * fs->ssize;
    const uint64_t totalBytes = clusterBytes * (fs->n_fatent - 2);
    const uint64_t freeBytes = clusterBytes * freeClusters;
    const uint64_t clusters = fs->n_fatent - 2;
    std::printf("sdformat: FAT volume: %llu B usable, %llu B free, "
                "cluster %llu B x %llu clusters\n",
                static_cast<unsigned long long>(totalBytes),
                static_cast<unsigned long long>(freeBytes),
                static_cast<unsigned long long>(clusterBytes),
                static_cast<unsigned long long>(clusters));

    if (s_card == nullptr) return;
    const uint64_t csdBytes = static_cast<uint64_t>(s_card->csd.capacity) *
                              s_card->csd.sector_size;
    const unsigned pct =
        csdBytes ? static_cast<unsigned>((totalBytes * 100ULL) / csdBytes) : 0;
    std::printf("sdformat: CSD capacity %llu B — FAT covers %u%%\n",
                static_cast<unsigned long long>(csdBytes), pct);
    if (totalBytes * 8 < csdBytes) {
        // The docs/HARDWARE.md 4.2 condition: the volume does not cover the
        // card. Before the 2026-11-16 reformat this fired on the owner's
        // card (5.0 MB vs 7.5 GiB).
        std::printf("sdformat: WARNING: FAT volume < 1/8 of CSD — the "
                    "partition does not cover the card\n");
    }
}

// --- commands -----------------------------------------------------------------

void printHelp() {
    std::printf("sdformat: commands (one ASCII line each):\n");
    std::printf("sdformat:   info    card CID/CSD, MBR partition table, "
                "FAT geometry\n");
    std::printf("sdformat:   format  erase everything on the card; write an "
                "MBR + FAT32\n");
    std::printf("sdformat:            volume covering the whole card "
                "(asks for confirmation)\n");
    std::printf("sdformat:   help    this text\n");
}

void cmdInfo() {
    if (s_card == nullptr) {
        std::printf("sdformat: no SD card initialised — re-seat the card "
                    "and reset\n");
        return;
    }
    // CID/CSD/OCR — this block carries the D5 card identity.
    sdmmc_card_print_info(stdout, s_card);
    printMbr();
    if (fatMount()) printFatGeometry();
}

void cmdFormat() {
    if (s_card == nullptr) {
        std::printf("sdformat: no SD card initialised — nothing to format\n");
        return;
    }
    uint64_t csdBytes = 0;
    csdBytes = static_cast<uint64_t>(s_card->csd.capacity) *
               s_card->csd.sector_size;
    std::printf("sdformat: *** DESTRUCTIVE — EVERYTHING ON THE CARD WILL BE "
                "LOST ***\n");
    std::printf("sdformat: this card currently holds the owner's bootable "
                "Linux Mint image (FAT boot +\n");
    std::printf("sdformat: ext4 root, docs/HARDWARE.md 4.2). 'format' "
                "replaces the whole partition\n");
    std::printf("sdformat: table with one MBR partition + FAT32 volume "
                "covering the full %llu B\n",
                static_cast<unsigned long long>(csdBytes));
    std::printf("sdformat: CSD capacity. Recoverable only by re-imaging the "
                "card on a PC.\n");
    std::printf("sdformat: NOTHING is written until you confirm. Type "
                "exactly %s (case-sensitive) on\n", kEraseToken);
    std::printf("sdformat: the next line to proceed; any other line "
                "cancels.\n");
    s_awaitingErase = true;
}

void doFormat() {
    std::printf("sdformat: ERASE confirmed — formatting...\n");
    if (s_fsMounted) {
        // Drop the stale FATFS view before rewriting the volume.
        f_mount(nullptr, kDriveStr, 0);
        s_fsMounted = false;
    }

    // FM_ANY with n_fat=2, auto align/root/cluster size. Verified in the
    // pinned ff.c: no FM_SFD -> f_mkfs first creates an MBR with a single
    // partition covering the whole card (create_partition, N_SEC_TRACK=63),
    // then formats FAT32 inside it (cluster count forces the FAT16->FAT32
    // upgrade at ff.c:6455-6456). FF_FS_EXFAT=0 rules exFAT out; the TRIM
    // hints f_mkfs issues on the way (ff.c:6244/6474) are unchecked, and
    // sdmmc_can_trim() rejects plain SD cards anyway, so they no-op.
    const MKFS_PARM opt = {static_cast<BYTE>(FM_ANY), 2, 0, 0, 0};

    const int64_t t0 = esp_timer_get_time();
    const FRESULT fr =
        f_mkfs(kDriveStr, &opt, s_mkfsWork, sizeof(s_mkfsWork));
    const long long ms =
        static_cast<long long>((esp_timer_get_time() - t0) / 1000);
    if (fr != FR_OK) {
        std::printf("sdformat: FORMAT FAIL — f_mkfs FRESULT %d after %lld ms\n",
                    static_cast<int>(fr), ms);
        return;
    }
    std::printf("sdformat: f_mkfs OK in %lld ms\n", ms);

    if (!fatMount()) {
        std::printf("sdformat: FORMAT FAIL — volume created but will not "
                    "mount\n");
        return;
    }
    printMbr();
    printFatGeometry();

    // Self-test at the FatFs layer this tool formatted: write, read back,
    // delete. VisionCam's own VFS path is re-proven separately by the Phase 0
    // smoke reflash that follows this run.
    static const char kPayload[] = "sd_format self-test ok";
    bool ok = false;
    do {
        if (f_open(&s_fil, kSelfTestPath, FA_WRITE | FA_CREATE_ALWAYS) !=
            FR_OK) {
            std::printf("sdformat: self-test: f_open(write) failed\n");
            break;
        }
        UINT written = 0;
        const FRESULT fw =
            f_write(&s_fil, kPayload, sizeof(kPayload), &written);
        f_close(&s_fil);
        if (fw != FR_OK || written != sizeof(kPayload)) {
            std::printf("sdformat: self-test: wrote %u of %u bytes "
                        "(FRESULT %d)\n",
                        static_cast<unsigned>(written),
                        static_cast<unsigned>(sizeof(kPayload)),
                        static_cast<int>(fw));
            break;
        }

        if (f_open(&s_fil, kSelfTestPath, FA_READ) != FR_OK) {
            std::printf("sdformat: self-test: f_open(read) failed\n");
            break;
        }
        char buf[sizeof(kPayload)] = {};
        UINT got = 0;
        const FRESULT frd = f_read(&s_fil, buf, sizeof(buf), &got);
        f_close(&s_fil);
        if (frd != FR_OK || got != sizeof(kPayload) ||
            std::memcmp(buf, kPayload, sizeof(kPayload)) != 0) {
            std::printf("sdformat: self-test: read back %u bytes, mismatch "
                        "(FRESULT %d)\n",
                        static_cast<unsigned>(got), static_cast<int>(frd));
            break;
        }

        if (f_unlink(kSelfTestPath) != FR_OK) {
            std::printf("sdformat: self-test: could not delete %s\n",
                        kSelfTestPath);
            break;
        }
        ok = true;
    } while (false);

    if (!ok) {
        std::printf("sdformat: FORMAT FAIL — volume created but self-test "
                    "failed\n");
        return;
    }
    std::printf("sdformat: self-test OK: wrote, read back and deleted "
                "VCSDFMT.TXT\n");

    uint64_t totalBytes = 0;
    DWORD freeClusters = 0;
    FATFS* fs = nullptr;
    if (f_getfree(kDriveStr, &freeClusters, &fs) == FR_OK && fs != nullptr) {
        totalBytes = static_cast<uint64_t>(fs->csize) * fs->ssize *
                     (fs->n_fatent - 2);
    }
    std::printf("sdformat: FORMAT OK — MBR + FAT32, %llu B usable, %lld ms\n",
                static_cast<unsigned long long>(totalBytes), ms);
}

}  // namespace

extern "C" void app_main(void) {
    std::printf("sdformat: SD format tool ready (115200) — commands: info, "
                "format, help\n");
    consoleInit();
    if (!consoleReady) {
        std::printf("sdformat: console init failed — aborting\n");
        return;
    }

    // Auto-run info so a plain serial capture always starts with the
    // pre-format state (the evidence requirement for docs/HARDWARE.md 4.2).
    if (cardInit()) cmdInfo();
    std::printf("sdformat: type 'help' for commands\n");

    char line[kLineMax];
    while (true) {
        if (!readLine(line, sizeof(line))) continue;
        const size_t len = std::strlen(line);
        if (len == 0) continue;

        if (s_awaitingErase) {
            if (std::strcmp(line, kEraseToken) == 0) {
                s_awaitingErase = false;
                doFormat();
                continue;
            }
            // Cancel first; a real command typed meanwhile still runs.
            s_awaitingErase = false;
            std::printf("sdformat: format cancelled — expected %s "
                        "(case-sensitive); %u chars ignored\n",
                        kEraseToken, static_cast<unsigned>(len));
        }

        if (std::strcmp(line, "info") == 0) {
            cmdInfo();
        } else if (std::strcmp(line, "format") == 0) {
            cmdFormat();
        } else if (std::strcmp(line, "help") == 0) {
            printHelp();
        } else {
            std::printf("sdformat: unknown command (%u chars) — try 'help'\n",
                        static_cast<unsigned>(len));
        }
    }
}
