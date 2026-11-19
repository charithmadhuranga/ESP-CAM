# Reference analysis — adopt / adapt / defer / reject

Companion to `SPEC.md` §1A. One entry per subsystem, written **after** reading
the pinned reference source and **in my own words** — never copied from R1
(AGENTS rules 31–36).

## Licence policy — owner decision D1 (recorded here as the exit gate requires)

**Option A — clean-room, chosen 2026-11-15.**

- R1 (`s60sc/ESP32-CAM_MJPEG2SD`, **AGPL-3.0**, `APP_VER 10.10.1`,
  commit `abe84d89fc74367c21b644cb5101c9b89fd4bfb7`) is a **behavioural
  reference only**. Ideas, architecture, measured numbers and failure lessons
  may be reused. Source may **not** be copied, vendored, or mechanically
  translated — specifically the 310-byte AVI header byte table and the
  `frameSizeData` table must be written fresh from the format spec, not lifted.
  R1 is never linked into a release build.
- R2 (`espressif/esp32-camera`, **Apache-2.0**, 2.1.9,
  commit `b3f7fe802f41f17fd7a104d517c53f1f62c8414d`) is a normal pinned
  dependency under either licence option; its licence notice ships in the
  release.
- Pinned checkouts and how to re-verify them: `reference/pins.txt`,
  `reference/fetch_references.sh --verify`.

If the owner ever selects Option B, this file and every entry below must be
revisited before any R1 code is reused.

---

## Board profile and pins — 2026-11-15

- **Studied:** R1 `camera_pins.h:187-206` @ abe84d8, R2 `driver/include/esp_camera.h` @ b3f7fe8
- **What it does / limits / failure modes:** The AI-Thinker camera is wired
  to a fixed set of GPIOs; several are input-only (34–39) and cannot be
  re-tasked. GPIO16 is reserved for PSRAM, GPIO1/3 are the UART, and GPIO
  0/2/12/15 are boot strapping pins — driving them wrongly at reset prevents
  boot (GPIO12 additionally selects flash voltage). GPIO4 is both the flash
  LED and SD D1.
- **Decision:** **adopt** the mapping as a compile-time board profile.
- **Reason:** Verified in three independent sources that agree exactly
  (SPEC §2.2, R1 `camera_pins.h`, owner-supplied board table). Nothing to
  reinterpret; the risk is duplication, so it lives only in
  `firmware/include/board_profile.h` (AGENTS rule 6).
- **Divergence from R1:** none in values. VisionCam adds the SD pin map,
  which SPEC §2.2 did not tabulate and which the owner supplied.
- **Evidence:** static cross-check complete; hardware confirmation
  **verified 2026-11-15** — camera (PID `0x0026`, JPEG at 1600×1200), SD
  (slot 1 pins 14/15/2) and GPIO4 all behaved exactly as this map predicts
  (`docs/HARDWARE.md` §5).

## Storage / SD card interface — 2026-11-15

- **Studied:** R1 `utilsFS.cpp:28,72,80-84` @ abe84d8, R2 n/a
- **What it does / limits / failure modes:** R1 mounts the card in 1-bit mode
  **by default** (`static bool use1bitMode = true;`), because on the original
  ESP32 1-bit is nearly as fast as 4-bit and frees GPIO4 and GPIO12. Immediately
  after `SD_MMC.begin(...)` it forces GPIO4 low, guarded by
  `CAMERA_MODEL_AI_THINKER`, with the comment that the SD library still
  initialises pin 4 *even in 1-line mode* — otherwise the lamp lights. If the
  mount fails R1 logs and continues rather than formatting.
- **Decision:** **adapt** — same1-bit default and same GPIO4-low-after-mount
  behaviour, but re-proven on the **ESP-IDF** SDMMC driver rather than
  Arduino's `SD_MMC`, because R1's workaround targets a different library.
  Auto-format stays permanently disabled.
- **Reason:** AGENTS rules 7 and 8 make 1-bit + GPIO4 a hardware constraint;
  R1's evidence is behavioural, not licence-relevant code.
- **Divergence from R1:** R1 exposes `formatIfMountFailed` as a user
  parameter; VisionCam never enables it (SPEC §2.3).
- **Evidence:** source confirmed at `utilsFS.cpp:81-84`;
  hardware confirmation **now done for the IDF driver, 2026-11-15: the lamp
  stayed OFF for the whole boot** — the ESP-IDF SDMMC 1-bit driver does not
  light GPIO4, so R1's workaround is Arduino-`SD_MMC`-specific and VisionCam's
  post-mount drive-low is belt-and-braces. **R1 side observed 2026-11-16:**
  owner saw a boot-window flash under R1 (consistent with `SD_MMC`
  initialising pin 4 at mount `00:00:00.572`, before R1's own lamp feature
  exists at `00:00:01.277`) plus a repeating blink afterwards, which the
  source timeline identifies as R1's status pattern while setup is
  incomplete — `docs/HARDWARE.md` §4, `docs/evidence/phase0-d2-r1-lamp-run.log`.

## SDMMC mount — two divergences from SPEC §2.3's snippet — 2026-11-15

SPEC §2.3 labels its example "Example starting point (verify against the
pinned ESP-IDF)". It has now been verified against the pinned **ESP-IDF
6.1.0**, and two places differ from the snippet as written.
`firmware/src/sd_bringup.cpp` implements both and points at this section
(task 0.4 step 18).

**1. Do not set `host.flags = SDMMC_HOST_FLAG_1BIT` (SPEC line 620).**

- **Redundant.** The bus width comes from `slot.width`, not from the flag:
  `sdmmc_fix_host_flags()` (`sdmmc_common.c:365-377`) reads the slot width and
  masks `host.flags` down to `SDMMC_HOST_FLAG_1BIT` when it is 1, then
  `sdmmc_init_host_bus_width()` (`sdmmc_common.c:222`) never requests 4-bit.
  `slot.width = 1` alone is what actually forces 1-bit (AGENTS rule 7).
- **Harmful.** Assigning that single flag **clears
  `SDMMC_HOST_FLAG_DEINIT_ARG`**, which `SDMMC_HOST_DEFAULT()` carries.
  `vfs_fat_sdmmc.c:447` branches on it when unmounting: with the flag,
  `deinit_p(slot)` tears down only this slot; without it, `host.deinit()`
  tears down the whole SDMMC peripheral. Keeping the default flags is the
  correct behaviour.

**2. `sdmmc_slot_config_t.clk / cmd / d0` are inert on ESP32.**

- ESP32 uses `SOC_SDMMC_USE_IOMUX`, not the GPIO matrix, so the driver sets
  `use_gpio_matrix = false` (`sd_host_sdmmc.c:1216`) and the pin fields never
  take effect — they only matter on targets that have a GPIO matrix. Setting
  them would be harmless but would imply control we do not have, so they are
  not set.
- The pins are fixed per slot: slot 1 = CLK 14, CMD 15, D0 2, D1 4, D2 12,
  D3 13 (`soc/sdmmc_pins.h:21-26`) — exactly the AI-Thinker SD pins — and
  slot 0 is the SPI flash (`sd_host_sdmmc.c:1231`), so being on slot 1 is not
  optional. `SDMMC_HOST_DEFAULT()` already resolves to slot 1
  (`sdmmc_default_configs.h:65`); `sd_bringup.cpp` sets `host.slot`
  explicitly anyway so a future IDF default change cannot silently move the
  bus onto the flash pins. SPEC's snippet relies on the default and does not
  set it.
- **Consequence for the board profile:** the SD pin constants in
  `board_profile.h` cannot be handed to the driver at all. They are
  compile-time documentation, enforced by `static_assert` against
  `SDMMC_SLOT1_IOMUX_PIN_*` so a wrong pin map fails the build instead of
  silently mounting nowhere.

- **Evidence:** IDF 6.1.0 source paths cited above; `sd_bringup.cpp`
  compiles. Mount behaviour on hardware **verified 2026-11-15**: slot 1,
  1-bit, 20,000 kHz, `SSR: bus_width=1`, write/read/delete self-test passed,
  no auto-format triggered (`docs/HARDWARE.md` §5).

## Motion detection cadence — 2026-11-15

- **Studied:** R1 `mjpeg2sd.cpp:27-28,123-131,489` and
  `motionDetect.cpp:718,729,762` @ abe84d8
- **What it does / limits / failure modes:** Motion is not a timer — it is a
  **frame counter** inside the capture loop. `doMonitor()` returns true every
  `FPS / moveStartChecks` frames while idle and every `FPS * moveStopSecs`
  frames while capturing; defaults are 5 and 2, giving ~5 checks/s idle and
  one check per 2 s while recording, and the stop value doubles as the
  post-motion hold time. Detection aborts above SXGA
  (`if (fsizePtr > FRAMESIZE_SXGA) return false`), and the JPEG is decoded to
  a 96×96 (or 64×64 under 3 MB PSRAM) grayscale sample whose size comes from
  `frameData[].sampleRate`.
- **Decision:** **adopt** the behaviour and expose both rates as
  configuration; **adapt** the implementation to ESP-IDF timing.
- **Reason:** SPEC §4 and §9 require exactly this behaviour, and the pinned
  source confirms it.
- **Divergence from R1:** VisionCam keeps motion running during continuous
  recording so events are still marked (R1 disables it); SPEC §1A.9 also
  rejects R1's "refuse playback while capturing".
- **Evidence:** source confirmed; performance `UNVERIFIED`.

## Camera initialisation — 2026-11-15

- **Studied:** R1 `mjpeg2sd.cpp` (`initCam`, warm-up, retry) @ abe84d8, R2
  `esp_camera.h:90-91` @ b3f7fe8
- **What it does / limits / failure modes:** Init must confirm the sensor PID
  is OV2640, discard ≥3 warm-up frames before trusting the image, and on
  failure power-cycle the camera via PWDN (GPIO32) and retry rather than
  giving up. Frame buffers live in PSRAM; the maximum JPEG buffer is
  `width*height/5`. `CAMERA_GRAB_LATEST` vs `CAMERA_GRAB_WHEN_EMPTY` decides
  whether a slow consumer sees stale frames or waits.
- **Decision:** **adopt** the sequence and limits (SPEC §4.1.1 already
  specifies it); **dependency adopt** R2 itself.
- **Reason:** R2 is Apache-2.0 and is the runtime driver — never a
  reimplementation target (SPEC §1A.3).
- **Divergence from R1:** none on init order.
- **Evidence:** R2 commit matches SPEC's recorded pin; driver smoke test
  **verified on hardware 2026-11-15** — PID `0x0026`, ≥3 warm-up frames,
  243,471 B JPEG at 1600×1200 (`docs/HARDWARE.md` §5).
- **Hardware finding (2026-11-15, changes the retry design):** R2's
  `camera_probe` **power-cycles PWDN itself at the start of every
  `esp_camera_init()`** (`esp_camera.c:204-216`: 10 ms high, 10 ms low), so
  an external PWDN hold placed before the call is released by the driver and
  cannot be used to force a failed attempt. Proven on the board: with PWDN
  held, attempt 1 still succeeded (`attempts=1`). The Phase 0 forced-failure
  test therefore sabotages attempt 1 with `pin_xclk = -1` (sensor gets no
  clock → real `ESP_ERR_NOT_SUPPORTED`), restoring XCLK on the retry — which
  then recovered at `attempts=2` (`docs/evidence/phase0-smoke-boot-11-*.log`).
  Consequence for the retry path: `powerCyclePwdn()` between attempts is
  still correct, but it is R2's own PWDN cycle plus ours, not the only
  power-cycle happening.

## Camera init / retry — pinned R2 facts vs its own documentation — 2026-11-15

- **Studied:** pinned esp32-camera 2.1.9 in `managed_components/` —
  `driver/include/esp_camera.h:187-188,203`,
  `driver/esp_camera.c:177-178,316-320,374-376,379-391`,
  `driver/include/sensor.h:21`, `driver/cam_hal.c:619`.
- **What was found:**
  - `esp_camera_init()`'s doc comment still says the module "can only be
    called once and there is no way to de-initialize"
    (`esp_camera.h:187-188`). **This is stale:** `esp_camera_deinit()` is
    declared (`esp_camera.h:203`), implemented (`esp_camera.c:379`) and nulls
    `s_state` (`esp_camera.c:387`).
  - Every *failure* path inside `esp_camera_init` reaches its `fail:` label,
    which already calls `esp_camera_deinit()` (`esp_camera.c:374-376`) — so
    retrying after a failed init works without extra help.
  - A *successful* init leaves `s_state` set, so a second `esp_camera_init()`
    returns `ESP_ERR_INVALID_STATE` from `camera_probe()`
    (`esp_camera.c:177-178`) unless deinit runs first. SPEC §4.1.1's PID
    check, warm-up discard and malformed-frame rejection all execute **after**
    a successful init, so those exit paths must deinit explicitly or a later
    bring-up can never recover. Taken at face value, R2's own doc comment
    would have forbidden the retry SPEC mandates.
  - `OV2640_PID` is **`0x26`**, not `0x2640` (`sensor.h:21`). SPEC asks for a
    PID check without quoting the constant — the trap is recorded here.
  - R2's default JPEG receive buffer is `width*height/5` (`cam_hal.c:619`) —
    **confirms** SPEC's "Proven design fact" instead of diverging from it.
- **Decision:** **adopt** the retry sequence with an explicit
  `cameraShutdown()` (= `esp_camera_deinit()`) before any re-init and on every
  post-init exit path (`camera_bringup.cpp`).
- **Evidence:** source lines cited above; on-device retry **verified
  2026-11-15**: sabotaged attempt 1 failed for a real reason, attempt 2
  recovered after `cameraShutdown()` + PWDN power-cycle
  (`attempts=2 -> OK`, `docs/evidence/phase0-smoke-boot-11-*.log`).

---

## §1A.5 matrix rows verified first-hand — task 0.4 step 18 (2026-11-15)

Only rows whose pinned source was actually opened are listed. Anything not
listed stays "as specified, not yet re-verified" — no row may be promoted on
the strength of SPEC's own text (AGENTS rule 26).

| §1A.5 row | Status | First-hand evidence |
|---|---|---|
| Camera driver → **adopt** R2 | verified | pinned 2.1.9 in `managed_components/`; facts in the camera entries above; single-copy build check passes (AGENTS rule 33) |
| Framework → **reject** Arduino | verified | R1 compiles only as an Arduino sketch: `arduino-cli` build = 1,734,516 B (88 %) flash / 102,888 B (31 %) RAM vs VisionCam IDF build 387,356 B (24.6 %) without the radio (1,041,544 B = 66.2 % with the smoke Wi-Fi module) — R1 has ≈231 KB free, no room to grow (`docs/HARDWARE.md` §3.2) |
| Frame buffers (R1: 4 × PSRAM, `GRAB_LATEST`) | verified | R1 `mjpeg2sd.cpp:20` `#define FB_CNT 4`, `:1070` `config.grab_mode = CAMERA_GRAB_LATEST`, `:1075` `config.fb_count = FB_CNT` — SPEC's claim is exact |
| SD mode → **adopt** 1-bit | verified | R1 `utilsFS.cpp:28` `use1bitMode = true`, `:80` `SD_MMC.begin("/sdcard", use1bitMode, ...)`; IDF side in the SDMMC entry above |
| Auth → **reject** Basic | verified | R1 `webServer.cpp:100-143`: pre-computes and caches the expected `Basic <b64>` value, then reads the request's `Authorization` header and compares — exactly the model SPEC replaces with sessions + CSRF |
| MQTT client → **adopt** esp-mqtt | verified | R1 `mqtt.cpp:14,36,46` uses `mqtt_client.h` / `esp_mqtt_client_publish`; VisionCam pins `espressif/mqtt@1.1.0` (`dependencies.lock`) and it builds |
| Camera init order → **adapt** | verified (source + hardware) | R1 `initCam` sequence read; R2-side retry facts in the entry above; on-device retry `attempts=2 -> OK` 2026-11-15 (`docs/evidence/phase0-smoke-boot-11-*.log`) |
| Motion cadence → **adopt/adapt** | verified | see Motion entry above (R1 `mjpeg2sd.cpp`, `motionDetect.cpp` line evidence) |

| Storage / free-space → **reject** R1's deletion loop | verified 2026-11-15 (source + hardware) | R1 `utilsFS.cpp:194-215` `checkFreeStorage`, called from `mjpeg2sd.cpp:374`; see the storage entry below |
| HTTP server config + MJPEG stream → **adapt** | verified 2026-11-16 | R1 `webServer.cpp:682-775` (`lru_purge`, 7 sockets, 4 KB stack), `streamServer.cpp:1-240` (framing, drop policy, stall abort); R2 README stream/capture handlers — see the Phase 1 streaming entry below |
| Wi-Fi lifecycle → **adapt** | verified 2026-11-16 | R1 `wifiEth.cpp:330-520, 640-720` (manual lifecycle, 5 s bound, ping-based link check) — see the Phase 1 Wi-Fi entry below |
| NULL-frame recovery → **adopt/adapt** | verified 2026-11-16 | R1 `mjpeg2sd.cpp:385-460, 588-600` (2 s gate, 10 attempts, 10 ms yield) + SPEC §9.1 — see the Phase 1 frame entry below |
| Logging / crash loop → **adopt** | verified 2026-11-16 | R1 `utilsLog.cpp:41-80, 755-775, 838-915` (RTC no-init ring, magic counter, task monitor) — see the Phase 1 logging entry below |
| Config store → **reject** mechanism | verified 2026-11-16 | R1 `prefs.cpp` (SD key/value file + Preferences mirror, save+reboot) vs SPEC §7 NVS-authoritative model — see the Phase 1 config entry below |

Rows **not** yet verified first-hand (still as-spec): AVI container layout
(partially — the write path was studied, the header byte table was not),
playback timing, timelapse, upload, WebDAV, HA discovery, and every
"defer/out of scope" row. They are re-verified when their phase begins.

## Storage and `checkFreeStorage` — 2026-11-15 (D2 run, source + hardware)

- **Studied:** R1 `utilsFS.cpp:194-215` (`checkFreeStorage`),
  `mjpeg2sd.cpp:374` (its only call site), `appSpecific.cpp` (`sdFreeSpaceMode`,
  `sdMinCardFreeSpace`); plus arduino-esp32 3.3.12 `SD_MMCFS::totalBytes()`.
- **What it does / limits / failure modes:** at every AVI close R1 computes free
  megabytes from the FAT volume and, in delete mode, removes the oldest day
  folder until free space reaches the configured minimum (default **100 MB**).
  The minimum is an absolute megabyte figure with no regard for the size of the
  volume it is compared against, and the deletion loop has no attempt cap.
- **Failure mode reproduced as a near-miss on this hardware:** the owner's card
  presents a **5,216,256 B** FAT volume (see the geometry entry below), so free
  space is 0–1 MB and can *never* reach a 100 MB target. In R1's default mode
  that loop therefore has no exit: it deletes the card's other content (here
  `/boot` and `/EFI`, the owner's) and then keeps calling `getOldestDir` on an
  empty card — a hang with the owner's data gone. It was **not** triggered here
  because `sdFreeSpaceMode` was set to 0 (warn-only) before the first
  recording; observed behaviour in that mode was a single
  `Space left 0MB is less than minimum 100MB` warning and recording stopping.
- **Decision:** **reject** as a model for VisionCam; **adopt** the "warn and
  stop" outcome. Phase 2/3 must: compute free space from the volume actually
  mounted, cap deletions per pass, never delete anything that is not a
  VisionCam recording, and treat "cannot reach the target" as a bounded,
  logged condition rather than a loop.
- **Divergence from R1:** R1's default deletes oldest-first without a bound;
  VisionCam keeps recordings it is not sure are its own.
- **Evidence:** `docs/evidence/phase0-d2-r1-serial-run.txt` (warning line);
  `docs/HARDWARE.md` §4.2.

## R1's AVI stats block — trustworthy only while writes succeed — 2026-11-15

- **Studied:** R1 `mjpeg2sd.cpp:226-267` (`saveFrame`), `:269-349` (close and
  stats block), `:374` (free-space check).
- **What it does / limits / failure modes:** frames are buffered in an 8 KiB RAM
  staging buffer and flushed to SD in `RAMSIZE` chunks. `vidSize` and `frameCnt`
  are incremented **unconditionally**, and the return value of `aviFile.write()`
  is never checked, so a short write is invisible to R1. The closing block then
  derives `File size`, `Actual FPS`, `Average SD write speed` and the per-frame
  timings from those same counters.
- **Observed on the owner's board:** one run reported `File size: 12.3MB`,
  `Actual FPS: 20.1` and `Average SD write speed: 3796 kB/s` while the file on
  disk was really **1,349,632 B** (the FAT directory entry said 1.3 MB and the
  full HTTP download was 1,349,632 bytes), with `used_bytes` running
  3.7 → 5.0 MB — the volume was simply full and the remainder of the writes
  failed silently. That 3796 kB/s figure is **discarded**.
- **Decision:** **adapt** the measurement, **reject** the mechanism. R1's
  formula is sound on a card with space (the reported rows here, 493–520 kB/s,
  each verified by `free_bytes` never reaching 0), so the *number* is usable;
  the *code path* that produced it is not. Phase 3's recorder must check every
  write's return value, count only bytes actually written, keep attempted vs
  written as separate figures, and stop the segment on a short write.
- **Evidence:** `docs/evidence/phase0-d2-r1-avi-stats-blocks.txt` runs 1–8 vs
  the discarded probe; `docs/HARDWARE.md` §4.2.

## FAT volume geometry — CSD, CID and BPB disagree — 2026-11-15

- **Studied:** ESP-IDF `esp_vfs_fat_sdmmc_mount` + `f_getfree` (VisionCam
  smoke), arduino-esp32 3.3.12 `SD_MMCFS::totalBytes()` (R1's figures),
  `sdmmc_card_print_info`.
- **What it does / limits / failure modes:** an SD card carries three capacity
  numbers, and only one of them constrains a filesystem: the CSD (what the card
  reports about itself), the CID product name (what the label claims), and the
  FAT BPB (the volume any FAT driver can actually allocate in). A card can be
  healthy in all three views only when they agree.
- **Observed on the owner's card:** CSD **8,053,063,680 B** (7.5 GiB),
  CID product name **`SD16G`**, FAT **5,216,256 B usable / 1,349,632 B free**
  (FAT12, 2048-byte clusters, 2547 clusters). arduino-esp32 and ESP-IDF FATFS
  returned the *same* geometry to the byte, so the 5 MB is in the card's own
  FAT — the formatted volume does not cover the card. Whatever is on it
  (`/boot`, `/EFI`, R1's `/data`) is all it can hold.
- **Decision:** **adopt** cross-checking at mount. VisionCam's SD smoke now
  prints the FAT volume alongside the CSD and warns when the volume is under
  one eighth of the CSD capacity, so this class of defect shows up in the boot
  log instead of being discovered during retention testing. Storage capacity
  planning uses the FAT figure only.
- **Consequences:** D5 cannot be closed from the driver alone (the printed
  label is still needed), and Phase 9's retention numbers cannot be sized on
  this card. VisionCam never auto-formats (SPEC §2.3), so any reformat is an
  owner action. — *Superseded 2026-11-16: the card was identified (genuine
  SanDisk 8 GB, pre-flashed with Linux Mint) and reformatted by
  `tools/sd_format/`; see the f_mkfs entry below.*
- **Evidence:** `docs/evidence/phase0-d2-vc-fat-crosscheck-boot.log`;
  `docs/HARDWARE.md` §4.2.

## Formatting a card without breaking the no-auto-format rule — 2026-11-16

- **Studied:** pinned ESP-IDF 6.1.0 FatFs — `ff.c` `f_mkfs` /
  `create_partition` (`N_SEC_TRACK`, system-ID selection, the
  FAT16→FAT32 upgrade at `ff.c:6455-6456`), `ffconf.h`
  (`FF_FS_EXFAT=0`, `FF_LBA64=0`, `FF_MULTI_PARTITION=1`, `FF_USE_MKFS=1`,
  `FF_FS_NORTC=0` + `get_fattime()` from `diskio_sdmmc.c:108`),
  `diskio_sdmmc.c` `CTRL_TRIM`, `sdmmc_cmd.c` `sdmmc_can_trim`; plus
  `esp_vfs_fat.h` (the `sdcard_init` helper is *not* public — it lives in
  `vfs_fat_internal.h`).
- **What it does / limits / failure modes:** `f_mkfs` with `FM_ANY` and no
  `FM_SFD` writes an MBR (single partition, start LBA 63, type chosen from
  the volume size) and then the filesystem inside it; on an 8 GB card the
  cluster count forces the FAT16→FAT32 upgrade, and with exFAT compiled out
  the result is always a FAT32 volume any FAT driver can mount. The `CTRL_TRIM`
  hints `f_mkfs` issues on the way are **unchecked** in `ff.c`, and
  `sdmmc_can_trim()` rejects plain SD cards anyway, so they cannot abort a
  format. `f_mkfs` needs a work buffer (4 KiB static here = internal SRAM =
  DMA-safe).
- **Decision:** **adopt** — but only inside `tools/sd_format/`, a standalone
  one-shot utility flashed *instead of* the firmware, gated behind a typed
  `ERASE` confirmation, and never linked into a release build. VisionCam
  itself keeps `format_if_mount_failed = false` forever (SPEC §2.3, AGENTS
  rule: never auto-format): the rule bans *automatic* formatting on mount,
  not a human-gated maintenance tool. Manual card init uses the public APIs
  (`sdmmc_host_init_slot` → `sdmmc_card_init` →
  `ff_diskio_register_sdmmc(0, card)`) because the convenient helper is
  internal; `sdmmc_card_t` embeds `card->host`, so host lifetime is
  straightforward.
- **Reason:** the owner's card shipped with a Linux Mint image whose layout
  FATFS cannot use (`docs/HARDWARE.md` §4.2), and the owner instructed that
  it be rewritten — the sanctioned path for a destructive operation is an
  explicit tool with confirmation, not firmware behaviour.
- **Evidence:** tool run on hardware 2026-11-16 —
  `docs/evidence/phase0-sd-format.log` (`FORMAT OK`, MBR `0x0C` at LBA 63,
  FAT32 8,051,032,064 B usable = 99 % of CSD, self-test OK, 2,791 ms),
  re-mounted by VisionCam in
  `docs/evidence/phase0-smoke-boot-12-postformat.log`.

## HTTP server and MJPEG streaming — 2026-11-16 (Phase 1)

- **Studied:** R1 `webServer.cpp:682-775` (server config + URI table),
  `streamServer.cpp:1-240` (`showStream`), `appGlobals.h:54-96` (budget
  constants) @ abe84d8; R2 README "JPEG HTTP Stream" / "JPEG HTTP Capture"
  examples @ b3f7fe8.
- **What it does / limits / failure modes:** R1 runs a single
  `esp_http_server` with `lru_purge_enable = true`, `max_open_sockets =
  HTTP_CLIENTS(3) + MAX_STREAMS(4) = 7`, and a 4 KB server stack on ESP32
  (7 KB on S3) — each sustained response (stream/playback/download) owns one
  of the 4 stream slots, so the socket count is a hard concurrency limit, not
  a tuning knob. The stream is `multipart/x-mixed-replace` with a fixed
  boundary; the capture task `memcpy`s each JPEG into a per-stream buffer
  **only if the consumer has cleared the previous one** (binary semaphore),
  otherwise that frame is skipped — that skip *is* R1's drop policy for slow
  consumers. The handler sends boundary + part header (`Content-Length`) +
  payload as chunks, sets no-cache headers, detects a gone client either from
  the send error or a `MSG_PEEK` probe on the socket, and aborts after 10
  consecutive frame timeouts (~12 s). Two failure modes R1 had to fix by
  hand: on exit the producer must be unblocked (buffer size cleared) or it
  deadlocks waiting for a consumer that will never return; and a stalled
  consumer must be logged rate-limited or it floods the log. R2's documented
  handler shape: set the multipart content type, then loop
  `fb_get → boundary → part header → payload → fb_return` with the return on
  **every** exit path, and log per-frame size/ms/fps with `esp_timer`;
  the capture handler is one `fb_get` + `image/jpeg` send + return.
- **Decision:** **adapt** the server configuration and framing; **adopt**
  R2's handler shape as the reference; **reject** two R1 behaviours.
- **Reason:** LRU purge, the documented socket budget, 4 KB stack, no-cache
  headers, disconnect detection and the stall abort are exactly what SPEC
  §6.0 asks for. But AGENTS rule 9 forbids any handler calling
  `esp_camera_fb_get`, so VisionCam's handler consumes **frame leases** from
  the frame manager instead of R1's memcpy-into-shared-buffer flow.
- **Divergence from R1:** (a) no `Access-Control-Allow-Origin: *` — a
  LAN-only device does not need wildcard CORS (SPEC §10); (b) the stream is
  cookie-authenticated (SPEC §6.3), never open, unlike R1 where Basic auth
  may be empty; (c) frame delivery is refcounted leases with PSRAM copies for
  slow consumers, not skip-if-busy memcpy.
- **Evidence:** source lines cited above; on-hardware behaviour
  `UNVERIFIED` until the Phase 1 stream test.

## Wi-Fi connection lifecycle — 2026-11-16 (Phase 1)

- **Studied:** R1 `wifiEth.cpp:330-520` (`startWifi`), `:640-720`
  (ping-based link monitoring) @ abe84d8.
- **What it does / limits / failure modes:** R1 manages the lifecycle
  manually — Arduino auto-reconnect is disabled, an event group carries
  `GOT_IP` / `DISCONNECTED` bits, each attempt is bounded to 5 s, and a
  failure logs status/RSSI/channel plus a diagnostic scan instead of
  blocking. Association alone is not trusted: a background `esp_ping`
  session pings the gateway every 30 s, because a station can stay
  *associated* to a router that no longer passes traffic; a ping timeout
  restarts the network stack. Modem sleep is on, TX 15 dBm, HT20 on both
  interfaces, hostname set for DHCP, `persistent(false)` to stop credential
  churn in flash. The crash-loop counter is cleared only after a real
  connection — a device that never reaches the network therefore keeps its
  "Crash loop detected" warning.
- **Decision:** **adapt** — same manual lifecycle and bounded attempts,
  re-implemented on `esp_netif`/`esp_event`/`esp_wifi` per SPEC §1A.10.
- **Reason:** VisionCam must work with **no internet** and no AP fallback in
  the default path; the plan specifies station mode with exponential backoff
  + jitter. Adopting the "association ≠ connected" principle: state is
  `Connected` only after a real `GOT_IP` (DESIGN principle), and RSSI is
  exposed for the status page.
- **Divergence from R1:** no AP+STA captive-portal provisioning flow in
  Phase 1 (credentials arrive via the `tools/nvs_provision` helper, same as
  Phase 0); no gateway-ping monitor yet — disconnect events + backoff first,
  link-health probing deferred to a later phase if field evidence shows
  associated-but-dead links; crash-loop clears when **core services** are up
  (camera + HTTP), not when Wi-Fi connects, because local operation is the
  product promise.
- **Evidence:** source lines cited; on-hardware behaviour `UNVERIFIED`
  until the Phase 1 Wi-Fi test.

## Frame acquisition cadence and NULL-frame recovery — 2026-11-16 (Phase 1)

- **Studied:** R1 `mjpeg2sd.cpp:385-460` (`processFrame`), `:588-600`
  (`captureTask`) @ abe84d8; R2 `esp_camera_fb_get/return` contract @ b3f7fe8.
- **What it does / limits / failure modes:** one capture task runs at a
  fixed period with `vTaskDelayUntil` (it catches up after a long cycle
  rather than drifting), and `processFrame()` is the only place that takes a
  frame. NULL recovery is time-gated: at most one attempt per 2 s, at most
  10 consecutive attempts, each failure yields 10 ms so the task watchdog is
  never starved, recovery is a sensor reset plus reapplying the frame size,
  and the counters reset the moment a valid frame arrives. Frames with
  `len == 0` or larger than the PSRAM buffer maximum are rejected and counted
  rather than passed downstream. When the attempt limit is hit R1 only logs
  "requires hard reset" — consumers keep running against a dead camera,
  which is exactly the gap SPEC §9.1 closes by raising `CameraFailed` and
  stopping consumers cleanly.
- **Decision:** **adopt** the cadence, the recovery ladder and the
  invalid-frame rejection; **adapt** the terminal failure path per SPEC §9.1
  (event + clean shutdown instead of an eternal log line), and the ladder
  gains the PWDN power-cycle + full re-init step (R1 only resets the
  sensor).
- **Reason:** SPEC §9.1 is explicit and derived from this code; the PWDN
  rung is already hardware-verified in Phase 0 (`attempts=2 -> OK`).
- **Divergence from R1:** terminal `CameraFailed` event with consumers
  stopped and state exposed; per-attempt recovery cost bounded (2 s gate)
  stays identical.
- **Evidence:** source lines cited; Phase 0 proved the PWDN re-init rung on
  hardware; the full ladder **verified on hardware 2026-11-17** as a
  standalone ladder (`docs/evidence/phase1-ladder-boot-01.log`: 10 attempts,
  5+5 rungs, one terminal `CameraFailed`, rule-3 reset, no spin) and **again
  through the live capture task**
  (`docs/evidence/phase1-frame-boot-11.log`: rung 1 ×5 + rung 2 ×5 gated to
  ~2 s apart, `attempts=10 rung1=5 rung2=5 errors=10`, `elapsed=21182 ms`,
  one terminal `CameraFailed`, `ladder reset=1`). 10 ms yields throughout
  — zero spinning.

## Logging, RTC ring log and crash-loop detection — 2026-11-16 (Phase 1)

- **Studied:** R1 `utilsLog.cpp:41-80` (RTC buffer + counters),
  `:755-775` (crash-loop logic), `:838-915` (task monitor) @ abe84d8.
- **What it does / limits / failure modes:** R1 keeps a ~6.6 KB message log
  in RTC slow memory with `RTC_NOINIT_ATTR` (the no-init attribute is what
  lets it survive a soft reset — RTC memory is only cleared on power-on),
  plus a `crashLoop` counter with a magic value written at every boot and
  cleared only when the device reaches a healthy state; if the magic is
  already present at boot, the device has rebooted before reaching health
  and it prints "Crash loop detected, check log (brownout)", using the
  reset reason to add the brownout hint. Panic backtrace addresses and the
  offending task are also parked in RTC memory for post-mortem. A task
  monitor periodically samples `vTaskGetSystemState` for CPU% and stack
  high-water marks.
- **Decision:** **adopt** all four concepts with a fresh implementation:
  a bounded RTC ring log, the magic-counter crash-loop detector (health
  point redefined for VisionCam), the brownout hint from `esp_reset_reason()`,
  and stack watermarks in the resource monitor.
- **Reason:** SPEC §8.4.1 asks for exactly this; R1's design is behavioural
  and the implementation will be written from the description above.
- **Divergence from R1:** health point = camera running + HTTP serving
  (Wi-Fi optional); log mirrored to serial in Phase 1 (SD/WS mirroring
  deferred — SD logging slows recording, R1's own lesson).
- **Evidence:** source lines cited; hardware `UNVERIFIED` until the Phase 1
  crash/restart test.

## Configuration store — 2026-11-16 (Phase 1)

- **Studied:** R1 `prefs.cpp:1-60, 135-230, 700-730, 846-866` @ abe84d8
  (plus the reference finding already recorded in SPEC §7).
- **What it does / limits / failure modes:** R1's settings live in a key/
  value file on SD loaded into a PSRAM vector, mirrored with `Preferences`
  for the few critical keys (SSID, Wi-Fi password, auth password); a version
  key detects obsolete files; secrets are masked with a filler in status
  output but the model needs "Save + reboot" for most changes because
  runtime state is a forest of globals updated from the vector. Failure
  modes: the SD file is the source of truth (removable media can be edited
  or absent), and split-brain between file and NVS needs reconciliation
  logic (`loadConfig` prefers one then falls back).
- **Decision:** **adopt** the lessons, **reject** the mechanism: NVS is the
  single authoritative store, versioned with explicit migration, secrets in
  separate keys never returned by GET (booleans like
  `password_configured` instead), and camera/network settings apply live
  without reboot (SPEC §7's update process: parse → validate → candidate →
  persist → apply → roll back on failure → return sanitized config).
- **Reason:** SPEC §7 mandates it; R1's file-on-SD model conflicts with
  AGENTS rule 17 (no secrets in logs/responses) and the local-first
  promise.
- **Divergence from R1:** everything above; R1's masking trick (`FILLSTAR`)
  is kept only as a reminder that GET responses must never echo secrets.
- **Evidence:** source lines cited; hardware `UNVERIFIED` until the Phase 1
  settings-persistence test.

## Camera driver ISR overflow warning vs flash-cache-off windows — 2026-11-17 (Phase 1)

- **Studied:** pinned esp32-camera 2.1.9 `driver/cam_hal.c:250-263`
  (`ll_cam_send_event`), `:119-129` (`CAM_WARN_THROTTLE`),
  `target/esp32/ll_cam.c:243` (DMA ISR); ESP-IDF fatal-errors doc (IRAM
  interrupt contract); upstream `master` `cam_hal.c` fetched 2026-11-17.
- **What it does / limits / failure modes:** the camera DMA ISR is allocated
  with `ESP_INTR_FLAG_IRAM`, so every instruction it executes and every byte
  it reads must be in IRAM/DRAM — the flash cache may be disabled at any
  instant by any other task doing flash I/O (NVS read/write, partition
  read). On a **queue overflow** (`ll_cam_send_event` failing to enqueue),
  the overflow branch runs `CAM_WARN_THROTTLE` → `ets_printf("EV-EOF-OVF")`
  — the format string lives in flash (`0x3f404898`, confirmed by disassembly
  of `ll_cam_send_event`: `l32r a11, … (3f404898 …)`), so if the overflow
  lands inside a cache-off window the ISR faults: `Cache error, Cache
  disabled but cached memory region accessed`, PC `0x40008047` (inside ROM
  `ets_printf`), backtrace joining `ll_cam_dma_isr` → `_xt_lowint1` and the
  interrupted task's NVS→`esp_flash_read` chain. The event queue is only
  `dma_half_buffer_cnt − 1` deep (`cam_hal.c:633`), so one or two frames of
  stall overflows it — the stall comes from radio-init bursts starving
  `cam_task` while `wifi_nvs_validate_lr_info` etc. read NVS with the cache
  off.
- **Reproduced on hardware 2026-11-17:** three byte-identical panics during
  step 14's `wifiStart()` with the camera streaming (PC/A0/A1 identical ×3;
  `docs/evidence/phase1-core-boot-03.log`). `EV-EOF-OVF` itself appears in
  the same window on a boot that survived — the overflow is deterministic
  during radio init; the *panic* only when the warning fires inside a
  cache-off window.
- **Upstream status:** the same `CAM_WARN_THROTTLE` macro is still present
  on `master` (fetched 2026-11-17) — **a pin bump cannot fix this**, and
  patching `managed_components/` would violate AGENTS rule 33 (never fork
  the driver).
- **Decision:** **defer to ordering mitigations** (no code change to the
  driver): (1) boot the **network before the camera** — the plan's task
  order 1.1.2 → 1.1.3 already does this, and this finding is the
  evidence-based reason for it: `esp_wifi_init`'s NVS reads must happen
  while no camera ISR exists; (2) never start/stop the radio while the
  camera streams — smoke step 14 now does `cameraShutdown()` →
  `wifiStart()` → `cameraBringup()` → `togetherCycle("wifi on")`, keeping
  the camera+SD+beacon concurrency check while removing the radio-init
  burst from the camera's lifetime; (3) config endpoints (task 1.2) must
  serialise NVS writes with camera framesize reconfiguration so a cache-off
  window never coincides with a framesize switch that stalls `cam_task`;
  (4) keep frame leases short (task 1.1.5) so `cam_task` drains the event
  queue promptly.
- **Reason:** the defect is in a pinned third-party ISR we may not modify;
  every viable fix is on our side of the boundary — ordering, serialising
  and bounded leases.
- **Evidence:** `docs/evidence/phase1-core-boot-03.log` (three panics +
  the surviving `EV-EOF-OVF`), addr2line/objdump decode recorded in this
  session, upstream `master` source check 2026-11-17.

## RTC noinit retention: EN reset clears it, SW/PANIC resets preserve it — 2026-11-17 (Phase 1)

- **Studied:** our own `ring_log.cpp` (`.rtc_noinit` symbols verified in the
  ELF at `0x50000000–0x5000101c`), observed across four consecutive boots.
- **What was found:** the RTC-no-init ring survives **software and panic
  resets** (`rst:0x3 (SW_RESET)` and `reset=PANIC` boot markers both
  retained predecessors' lines — proven 3× in one capture: boots 2, 3 and 4
  each dumped the full prior history including crash markers), but an
  **EN-pin reset** (DTR pulse / upload's hard reset, which the ROM reports
  as `POWERON_RESET`) **clears** `.rtc_noinit` — observed twice (fresh
  `boot=1` after upload; fresh `boot=1` after an EN pulse with no flash in
  between). Healthy-to-healthy SW-reset retention (boot counter increments
  with history intact, no crash in between) is *not yet directly
  demonstrated* — no restart trigger exists yet; re-test when task 1.2's
  restart endpoint lands.
- **Decision:** **adopt as-is** with the documented semantics: the ring log
  is crash evidence, and crashes on this chip are SW/PANIC resets, which
  preserve it. `ringLogInit()`'s magic check correctly re-initialises after
  an EN/power-on clear, so nothing changes in code. The operational rule:
  **an EN reset (re-flash, manual reset button wired to EN) starts a fresh
  log — do not read `boot=1` after a flash as "retention broken".**
- **Reason:** this is chip behaviour, not a defect; documenting it prevents
  a future session from misdiagnosing retention.
- **Evidence:** `docs/evidence/phase1-core-boot-02.log` (EN clear) and
  `phase1-core-boot-03.log` (SW/PANIC retention ×3).

## Network manager — station mode on hardware, and a credential-path bug — 2026-11-17 (Phase 1)

- **Studied:** our own `firmware/src/network/` against the pinned ESP-IDF
  6.1 APIs (`esp_wifi` event contract, `esp_netif_sntp`), then the board.
- **What was built:** `network_manager.cpp` — one event handler on the
  default event loop (state + events only), one reconnect task owning
  `esp_wifi_connect()` with exponential backoff (1 s doubling to 32 s, plus
  0..500 ms hardware-RNG jitter, counter reset on `IP_EVENT_STA_GOT_IP`),
  `Connected` only on a real GOT_IP, SNTP started on the first GOT_IP.
  Credentials come from NVS (SPEC 7): our `visioncam` namespace first, then
  the Phase 0 `ESP-CAM_MJPEG` keys written by `tools/nvs_provision` as a
  one-time migration source. Nothing but lengths is ever logged.
- **What hardware proved (2026-11-17,
  `docs/evidence/phase1-net-boot-01.log`):** the radio starts at
  `networkManagerInit()`, **before** camera bring-up (1231 ms vs 2051 ms) —
  the ordering the ISR-hazard entry above demands — and no panic occurs
  even while the camera runs under real association traffic. Connection is
  real: `GOT_IP 192.168.43.171 (rssi -68 dBm)` (same address R1 used in the
  D2 run), first attempt failed, backoff retried at 2 s and connected —
  the exponential schedule and the diagnostics counters all behave.
  Degraded mode works too: with no credentials the driver is never touched
  and the device stays local-only (`phase1-net-boot-04.log`).
- **A companion hazard observation:** `cam_hal: EV-EOF-OVF` appeared once
  in boot-01 during camera warm-up while the radio was associating — the
  same overflow warning as the panic root cause, without the cache-off
  window this time. Reinforces mitigation (3)/(4) of the ISR entry: keep
  flash writes (NVS commits) out of the camera's lifetime and let
  `cam_task` drain promptly. The run stayed panic-free and cache-error-free.
- **A defect found and fixed (recorded so it is not re-introduced):** the
  migration fallback was unreachable in the "our namespace exists but its
  keys are missing" state because `nvs_get_str` returns the **NVS-specific**
  `ESP_ERR_NVS_NOT_FOUND` (0x1102 family) and the code compared it against
  the **generic** `ESP_ERR_NOT_FOUND` (0x0003 family) — different numbers,
  so a plain "key missing" read propagated as a fatal credential error
  (`phase1-net-boot-02.log`). Any NVS result-code comparison in this
  project must use the `ESP_ERR_NVS_*` symbol.
- **Data-loss lesson — NVS partition-view mismatch:** the credentials used
  for boot-01 were **lost** afterwards. The provisioning tool boots with a
  20 KB nvs table at 0x9000 while the firmware boots with a 92 KB table at
  the same offset; alternating them while writing NVS left the page chain
  inconsistent between the two views, and the firmware's recovery path
  (`nvs_init` failure → `nvs_flash_erase`, SPEC 7's single erase case)
  erased the partition — credentials included. Rule recorded in
  `tools/nvs_provision/README.md`: after any tool with a different
  partition table, re-provision and confirm `Connecting`/`Connected` on the
  next firmware boot. **The owner must re-provision the LAN credentials
  before the next connection test** — they exist nowhere else (the
  workstation reads them from NetworkManager only while on that network).
- **Evidence:** `phase1-net-boot-01.log` (connect + backoff + GOT_IP),
  `-02.log` (NVS error-code bug), `-04.log` (local-only mode);
  `tools/nvs_provision` now has a confirmation-gated `vcunset` for
  re-testing the migration. SNTP sync line itself: `UNVERIFIED`.

## Deliberate divergences from R1 — do not "fix" these back

| Area | R1 | VisionCam |
|---|---|---|
| Framework | Arduino sketch | Native ESP-IDF via PlatformIO (AGENTS rule 37) |
| Auth | Basic over HTTP, `strcmp` | Sessions, forced password change, salted hash, CSRF |
| UI delivery | Files on SD, downloaded from GitHub | Embedded in firmware, React + TS |
| Playback during capture | **Refused** (`mjpeg2sd.cpp:656`) | Never stalls recording; else `503 PLAYBACK_BUSY` |
| Crash safety | Header/index written only at close | Short segments + provisional header + boot recovery scanner |
| Motion during continuous recording | Disabled | Keeps running so events are marked |
| MQTT topics | `homeassistant/sensor/{hostname}` | `visioncam/{device_id}` stable IDs |
| Images over MQTT | Published on motion | Never by default; authenticated URL instead |
| Folder listing | Whole listing in one 32 KB `JSON_BUFF_LEN` buffer (`appGlobals.h:58`) | Paginated — conflicts with AGENTS rule 14 |
| Config | Key/value file + Preferences, save + reboot | NVS authoritative, versioned, live apply |
| Auto-format SD | Exposed as a parameter | Never (SPEC §2.3) |

## Evidence discipline

Every row above marked `UNVERIFIED` stays that way until a serial log or tool
output exists (AGENTS rule 26). R1's published performance numbers are
**targets**, not measurements, until reproduced on the owner's board —
`.opencode/plans/owner-decisions.md` D2.

## AVI recording engine — 2026-11-19

- **Studied:** R1 `src/avi.cpp` @ abe84d8 (header template, index buffer,
  header patching), `src/mjpeg2sd.cpp:226-267` (`saveFrame`),
  `:269-383` (`closeAvi`), `:98-121` (`openAvi`), `:385-588` (`processFrame`)
  @ abe84d8.
- **What it does / limits / failure modes:**
  - Container: fixed 310-byte RIFF/AVI header template at offset 0, `movi`
    list of `00dc` JPEG chunks (4-byte marker + 4-byte size + JPEG content),
    each padded to a 4-byte boundary, trailing `idx1` index (16 bytes per
    frame: 4-byte marker + 4-byte flags + 4-byte offset + 4-byte size).
  - Index built in a PSRAM buffer sized `(maxFrames+2)*16` (maxFrames=20000
    → ~320 KB PSRAM). `buildAviIdx()` aborts with `doRestart("Need to reboot
    if max frames changed")` if the buffer would overflow — no segment
    rotation, no recovery.
  - Sector-aligned staging: double-buffer `iSDbuffer[(RAMSIZE+CHUNK_HDR)*2]`
    (R1's `RAMSIZE` = 8192), writes whole buffers only.
  - Temporary name (`AVITEMP` = `current.avi`) → renamed to final
    `dateTime_framesize_FPS_duration_count.avi` only after header+index
    written. Per-day folders.
  - Header stores **measured** FPS (`frameCnt / elapsed`), not requested FPS.
  - Minimum duration: segments shorter than `minSeconds` (default 5 s) are
    deleted unless event-marked.
  - **Crash hazard:** header + index are written **only at close** (`closeAvi`
    seeks to offset 0 and patches the header). A power loss during recording
    leaves an unplayable `current.avi` with no recovery path.
  - **Playback refusal:** `openSDfile()` checks `stopPlayback` / `isCapturing`
    and refuses to play while capturing (`mjpeg2sd.cpp:656`).
  - No audio support (VisionCam never records audio — SPEC §78).
- **Decision:** **adopt** the RIFF/AVI on-disk layout and sector-aligned
  write strategy; **adapt** everything else.
- **Reason:** SPEC §4.6 mandates short segments (default 60 s), provisional
  header at segment start, boot-time crash recovery scanner, measured-FPS
  header, min-duration deletion, per-day folders with opaque-ID API, and
  playback that never stalls recording. R1's "header only at close" and
  "refuse playback during capture" are deliberate VisionCam divergences
  (recorded in the divergence table above). The 310-byte header byte table
  must be written fresh from the public RIFF/AVI spec, not copied (licence
  rule).
- **Divergence from R1:**
  1. Short segments (default 60 s) vs R1's single long segment up to
     `maxFrames` (20000 frames).
  2. Provisional header at segment start + boot-time recovery scanner for
     `*.tmp` segments (R1 has neither).
  3. Segment rotation at duration/size/frame-count cap instead of `doRestart`
     on index overflow.
  4. Playback allowed concurrently with recording (lower priority, throughput
     share check, `503 PLAYBACK_BUSY`) vs R1's refusal.
  5. No `doRestart` / reboot on error — segment rotation instead.
  6. Opaque recording IDs (`rec-…`) via API, never filesystem paths.
  7. No audio support.
- **Evidence:** Source confirmed at `src/avi.cpp:45-67` (header template),
  `:102-103` (`IDX_ENTRY=16`), `:115` (`(maxFrames+2)*IDX_ENTRY` PSRAM
  index), `:269-383` (`closeAvi` writes header only at close),
  `:656` (playback refusal). R1 baseline FPS numbers in `docs/HARDWARE.md`
  §4 (QVGA 30.8, VGA 20.6, SVGA 20.4, XGA 5.3, SXGA 6.2 @ quality 12) are
  the acceptance bar — VisionCam must match within 10 %.

## Playback path — 2026-11-19

- **Studied:** R1 `src/mjpeg2sd.cpp:654-784` (`openSDfile`, `getNextFrame`),
  `:786-795` (`stopPlaying`) @ abe84d8.
- **What it does / limits / failure modes:**
  - Server-side replay: device reads AVI from SD in `RAMSIZE` (8 KB) blocks,
    walks `00dc` chunks, streams frames as `multipart/x-mixed-replace` MJPEG
    at the recorded FPS. Browser renders in an `img`/canvas — no client-side
    AVI parser.
  - Double-buffer overlap technique: `memcpy(iSDbuffer, iSDbuffer+RAMSIZE,
    CHUNK_HDR)` prevents a JPEG marker being split across buffer boundaries.
  - Pacing via `vTaskDelayUntil(&lastWake, period)` where
    `period = pdMS_TO_TICKS(recFPSinterval)` — playback at recorded FPS.
  - Seeking by `start_frame` / `start_ms` from `idx1` (conceptually; R1's
    `getNextFrame` walks sequentially from header offset).
  - Playback stats logged (frames, SD read speed, busy %).
  - Playback refused while capturing (`stopPlayback` / `isCapturing` check).
  - Single stream slot per playback.
- **Decision:** **adopt** server-side replay + double-buffer overlap +
  `vTaskDelayUntil` pacing + `idx1`-based seek; **adapt** to ESP-IDF +
  VisionCam concurrency rules.
- **Reason:** SPEC §4.6 specifies server-side replay as primary, sector-multiple
  block reads, `00dc` chunk walking, timed `multipart/x-mixed-replace` MJPEG
  at recorded FPS, seek via `start_frame`/`start_ms` from `idx1`, and
  playback that never stalls recording (`503 PLAYBACK_BUSY` if throughput
  share exceeded).
- **Divergence from R1:**
  1. Playback runs at lower priority than the storage writer; allowed only
     within a configured share of measured card throughput; `503 PLAYBACK_BUSY`
     otherwise (R1 refuses outright).
  2. Playback consumes one of the limited stream slots (max 2).
  3. Frame delivery uses VisionCam's frame-lease mechanism (AGENTS rule 9)
     instead of R1's direct `esp_camera_fb_get`/memcpy flow.
  4. No playback during capture stalling — concurrent allowed.
- **Evidence:** Source confirmed at `src/mjpeg2sd.cpp:654-784`. On-hardware
  playback test: UNVERIFIED (Phase 3).

## Motion detection engine — 2026-11-19

- **Studied:** R1 `src/motionDetect.cpp` @ abe84d8 (`analyzeMotionFrame`,
  `detectBlockSize`, `motionVal`, `detectMotionFrames`, `moveThreshold`,
  light level accumulation, debug change map) and
  `src/mjpeg2sd.cpp:123-131` (`doMonitor` frame-counter cadence),
  `:27-28` (moveStartChecks=5, moveStopSecs=2) @ abe84d8.
- **What it does / limits / failure modes:**
  - Decodes JPEG to a small grayscale bitmap (R1: 96x96 RESIZE_DIM) and
    compares it with the previous sample using per-block Sum of Absolute
    Differences (SAD). The downscale acts as the noise filter.
  - Per-pixel intensity threshold (configurable sensitivity `motionVal`);
    changed-pixel ratio vs `minimum_changed_ratio`; temporal smoothing,
    settle counter, cooldown.
  - Two sample rates (frame-counter based): higher while idle (~5 checks/s
    via `FPS / moveStartChecks`), lower while recording (1 check/2 s via
    `FPS * moveStopSecs`, which doubles as post-motion hold time).
  - Detection aborted above SXGA (decoder limit).
  - Ambient light level derived from the same decode; hysteresis +
    settle counter for night-mode decisions.
  - Debug change map (changed pixels highlighted red) for threshold
    calibration.
  - Trigger-source bitmask (camera, manual, future PIR) recorded on events.
  - R1 disables motion during continuous recording; VisionCam keeps
    detecting so events can still be marked (deliberate divergence).
- **Decision:** **adopt** the algorithm and cadence; **adapt** to ESP-IDF
  (R2 JPEG decode, FreeRTOS task instead of Arduino loop, event bus
  instead of global flags).
- **Reason:** SPEC §4.3 and §4.3.1 specify exactly this behaviour. The
  96x96 grayscale downscale is small and fixed — no unbounded allocation
  (AGENTS rule 13). The event bus is already bounded with drop-oldest
  overflow (AGENTS rule 12).
- **Divergence from R1:**
  1. Motion runs during continuous recording (R1 disables it).
  2. Implemented as a FreeRTOS task subscribing to frame leases, not an
     Arduino loop inline with capture.
  3. Events published to the typed event bus instead of global boolean
     flags.
  4. JSONL event index records with `detections[]` and
     `notification_status` (SPEC §4.8).
  5. No ML/TinyML integration in Phase 4 (external AI is Phase 6).
- **Evidence:** Source confirmed at `src/motionDetect.cpp:406-520`
  (analyzeMotionFrame), `:27-34` (motion parameters),
  `:478` (moveThreshold formula). On-hardware motion test: UNVERIFIED
  (Phase 4).

## Timelapse — 2026-11-19

- **Studied:** R1 `src/mjpeg2sd.cpp:133-207` (`timeLapse`) @ abe84d8.
- **What it does / limits / failure modes:**
  - Writes time-lapse frames directly into an AVI as they are captured,
    using the same header/index code as recording, with a separate index
    buffer. Stops after a configured duration.
  - `tlSecsBetweenFrames` controls the capture interval in seconds.
  - `tlDurationMins` controls total capture duration.
  - Filename format: `dateTime_framesize_FPS_duration_T.avi` (`_T` suffix
    for timelapse). Written to `TLTEMP` first, then renamed on close.
  - `timeLapseOn` flag gates capture; `intervalMark = tlSecsBetweenFrames *
    FPS` determines when to capture a frame.
  - Captures at full recording FPS and picks one in N frames — keeps the
    camera streaming continuously (power/heat concern).
- **Decision:** **adopt** direct-to-AVI as the default mode (no heavy
  generation step, avoids CPU/SD contention); **adapt** to ESP-IDF +
  VisionCam concurrency rules.
- **Reason:** SPEC §4.7 specifies direct-to-AVI as the default with an
  optional frames+generate mode. R1's direct-to-AVI approach avoids a
  heavy generation step.
- **Divergence from R1:**
  1. Direct-to-AVI uses the Phase 3 AVI writer code (shared header/index).
  2. Low-rate camera mode (`fb_count=1` on demand) when interval is long
     and no stream/recording active — R1 keeps camera streaming
     continuously.
  3. Projects with `project.json` metadata (name, state, schedule,
     resolution, quality, playback FPS, retention, counters, timestamps).
  4. Time synchronisation required before starting (unsynchronized →
     refuse).
  5. Coexistence with continuous recording allowed only if measurements
     show headroom; otherwise mutual exclusion with clear UI message.
  6. Opaque project IDs (`tl-…`) via API, never filesystem paths.
  7. Background generation job with progress/cancellation for frames+
     generate mode.
- **Evidence:** Source confirmed at `src/mjpeg2sd.cpp:133-207`. On-hardware
  timelapse test: UNVERIFIED (Phase 5).

## Evidence discipline

Every row above marked `UNVERIFIED` stays that way until a serial log or tool
output exists (AGENTS rule 26). R1's published performance numbers are
**targets**, not measurements, until reproduced on the owner's board —
`.opencode/plans/owner-decisions.md` D2.
