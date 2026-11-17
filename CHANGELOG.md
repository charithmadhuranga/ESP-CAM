# Changelog

All notable changes to VisionCam Pro are recorded here.
Format follows [Keep a Changelog](https://keepachangelog.com/); versions are
tagged once Phase 9 release criteria pass.

## [Unreleased]

### Added

- **Phase 1 task 1.1.1 — core services (2026-11-17).** New
  `firmware/src/core/` + headers: `coreInit()` boot order (RTC ring log
  first, so a crash later in the boot cannot destroy the evidence), typed
  bounded **event bus** (depth 16, drop-oldest, dispatcher task), **RTC RAM
  ring log** (4096 B `.rtc_noinit`, crash-loop counter with health point at
  *end of boot*), **resource monitor** (heap / largest block / min heap /
  PSRAM / uptime / reset reason / task stack watermarks, thresholds 32 KB
  free / 16 KB largest internal). Evidence:
  `docs/evidence/phase1-core-boot-01..04.log`; `docs/TEST_PLAN.md` 1.1.1a–e.
- **Two hardware findings recorded in `docs/REFERENCE_ANALYSIS.md`
  (2026-11-17):**
  (1) *esp32-camera 2.1.9 ISR hazard* — starting the Wi-Fi radio while the
  camera streams overflows the camera's 1–3-deep DMA event queue; the ISR's
  overflow warning reads a flash-resident format string while the flash
  cache is off (radio-init NVS reads) → `Cache error` panic, reproduced
  **3× byte-identical** (`phase1-core-boot-03.log`). Upstream `master` has
  the same unfixed macro, so no pin bump helps and patching
  `managed_components/` is forbidden (rule 33). Mitigations adopted:
  network-before-camera boot order, never start the radio while the camera
  streams (smoke step 14 now does camera-down → `wifiStart()` → camera-up —
  verified clean in `phase1-core-boot-04.log`), serialise NVS writes with
  camera framesize reconfig (task 1.2), short frame leases (task 1.1.5).
  (2) *RTC noinit retention* — EN-pin resets (upload / manual reset) clear
  `.rtc_noinit`; SW/PANIC resets preserve it (proven 3×). Operational rule:
  `boot=1` after a flash is **not** broken retention.
- **Crash-loop blind spot found and fixed:** health used to be marked when
  the camera came up (~0.8 s) but crashes occurred later (~2.2 s), so
  `abnormal_boots` stayed 0; `ringLogMarkHealthy()` moved to after the boot
  verdict (`phase1-core-boot-03.log` vs `-04.log`).

- **Phase 0 closed — all owner questions answered (2026-11-16).** The three
  D2 follow-ups are recorded with evidence: **GPIO4 lamp under R1 observed**
  (boot-window flash consistent with Arduino `SD_MMC` initialising pin 4 at
  mount `00:00:00.572` — before R1's own lamp feature exists at `00:00:01.277`,
  so the later repeating blink is R1's status pattern, not SD activity; the
  ESP-IDF driver still provably leaves the lamp off for the whole boot —
  `docs/evidence/phase0-d2-r1-lamp-run.log`); **D5 answered** (genuine
  SanDisk 8 GB, no speed-class marking visible; card reformatted the same
  day); **weak-supply brownout drill skipped by owner decision**
  (normal-supply behaviour verified brownout-free). `docs/HARDWARE.md` §1/§3.6/§4,
  `docs/TEST_PLAN.md` 0.11, plans updated.
- **`tools/sd_format/` — one-shot SD card formatter (2026-11-16).** The
  owner's 8 GB SanDisk card shipped with a bootable Linux Mint image (the
  "5 MB FAT volume" of `docs/HARDWARE.md` §4.2 was its EFI partition; the
  rest was ext4, invisible to FATFS). VisionCam never formats a card
  (SPEC §2.3 keeps `format_if_mount_failed = false`), so the rewrite lives
  in this standalone tool: flash it, run `format`, type `ERASE`
  (case-sensitive, nothing is written before it), then flash the firmware
  back. FatFs `f_mkfs` from the pinned ESP-IDF 6.1.0 wrote **MBR (one
  partition, `0x0C`, start LBA 63) + FAT32 across the whole card:
  8,051,032,064 B usable = 99 % of the CSD**, 32 KB clusters, format
  2,791 ms, write/read/delete self-test OK. Evidence:
  `docs/evidence/phase0-sd-format.log`; VisionCam reflash re-mounts it with
  no coverage warning and a green verdict line
  (`docs/evidence/phase0-smoke-boot-12-postformat.log`); TEST_PLAN row 0.13;
  analysis in `docs/HARDWARE.md` §4.2.1 and the `docs/REFERENCE_ANALYSIS.md`
  f_mkfs entry. Clean-room (D1): no R1 code.
- **D2 R1 baseline — measured 2026-11-15.** R1 (`ESP32-CAM_MJPEG2SD`
  10.10.1 @ `abe84d8`) was flashed **unmodified** and driven over HTTP while a
  passive 115200 capture recorded its own stats block at every recording close.
  `docs/HARDWARE.md` §4 now holds the acceptance bar: recording FPS 30.8
  (QVGA), 20.6 (VGA), 20.4 (SVGA), 5.3 (XGA), 6.2 (SXGA) at quality 12, plus
  VGA at quality 8 (20.7) and 16 (20.4); SD write 493–520 kB/s
  (0.48–0.51 MB/s); free heap 72–75 KB, largest block 41–43 KB, lifetime min
  53,504 B, pSRAM free 914,892 B; zero brownout/panic/WDT events. Evidence:
  `docs/evidence/phase0-d2-r1-avi-stats-blocks.txt`,
  `docs/evidence/phase0-d2-r1-serial-run.txt`.
- **`tools/nvs_provision/`** — small pinned ESP-IDF helper (no R1 code) that
  writes R1's two documented Preferences keys (`ST_SSID`/`ST_Pass` in namespace
  `ESP-CAM_MJPEG`) so R1 joins the owner's LAN and the baseline can be measured
  from this workstation instead of a second device. Reads the passphrase from
  NetworkManager over UART; it is never printed, logged or put on a command
  line (rule 17) and nothing credential-bearing is committed. Also carries a
  `join` diagnostic that prints the raw `WIFI_REASON_*` code — which is how
  R1's first post-provision failure was shown to be a transient association
  issue, not bad credentials. Procedure: `docs/HARDWARE.md` §3.7.
- **FAT volume diagnostic in the SD smoke test** — `sd_bringup.cpp` now prints
  the mounted volume (usable/free bytes, cluster layout) next to the CSD and
  warns when the volume is under one eighth of the CSD capacity.
- **Storage finding** (`docs/HARDWARE.md` §4.2): the owner's card reports
  **CSD 8,053,063,680 B** and CID product name **`SD16G`**, but both FATFS
  implementations (Arduino's and IDF's) report a **5,216,256 B FAT12 volume
  with 1,349,632 B free** — the formatted volume does not cover the card.
  D5 and Phase 9 retention sizing stay open on it.

- **Analysis and planning** (`.opencode/plans/`)
  - `00-analysis.md` — inventory, 15 hard constraints, authority order,
    8 in-spec supersessions, mockup and reference analysis, gaps G1–G16,
    risks, traceability map, and a 34-row spot-check of SPEC claims against
    the pinned R1/R2 source.
  - `README.md`, `owner-decisions.md`, `11-acceptance-matrix.md`.
  - Ten phase work orders: `01-phase-0-baseline.md` …
    `10-phase-9-hardening-release.md`.
- **Agent instructions** — root `AGENTS.md` (SPEC §14's 37 rules), scoped
  `firmware/AGENTS.md` and `webui/AGENTS.md`, root `SKILL.md` pointer, and six
  skills under `.opencode/skills/` (`visioncam-dev`, `visioncam-build-flash`,
  `visioncam-reference-first`, `visioncam-webui`, `visioncam-api-contract`,
  `visioncam-hardware-validation`).
- **Owner decisions D1, D2, D3** recorded (clean-room licence; R1 baseline run
  during Phase 0; FPS parity tolerance **10 %**).
- **Reference pinning** — `reference/README.md`, `reference/pins.txt` (tree
  SHA-256 + upstream commit for both refs), `reference/fetch_references.sh`
  with `--verify`.
- **Repository baseline** — `git init`, `.gitignore`, directory skeleton,
  initial commit `edfbb1e`.
- **Phase 0 build** — `platformio.ini`, root + component `CMakeLists.txt`,
  `sdkconfig.defaults`, `firmware/include/board_profile.h`,
  `firmware/src/main.cpp` smoke test.
- **Pinned IDF components** (task 0.2) — `firmware/src/idf_component.yml`
  (`espressif/esp32-camera@2.1.9`, `espressif/mqtt@1.1.0`), committed
  `dependencies.lock` (also pins transitive `espressif/esp_jpeg@1.3.2`),
  `managed_components/` gitignored.
- **Rule 33 single-copy check** in the root `CMakeLists.txt` — fails the
  configure if zero or more than one component provides `esp_camera.h`.
  Both the pass and the fail path were exercised with a real fixture.
- **Camera + SD smoke modules** (task 0.3 steps 11–13) —
  `camera_bringup.{h,cpp}` (SPEC §4.1.1: PSRAM-sized init, 3 attempts with
  PWDN power-cycle, OV2640 PID check, ≥3 warm-ups, rule-7 frame check),
  `sd_bringup.{h,cpp}` (SPEC §2.3: SDMMC slot 1, 1-bit, no auto-format,
  write/read/delete self-test, GPIO4 lamp probe), rewritten `main.cpp`.
- **Partition table** (deliverable 5) — `partitions.csv`: two 1.5 MB OTA
  slots, 8 KB `otadata`, 4 KB `phy_init`, 92 KB `nvs`, 884 KB `storage`.
  Exact fit to `0x400000`, alignments and overlaps validated.
- **Docs** — `docs/adr/0001-toolchain.md`, `docs/HARDWARE.md` (R1 baseline
  procedure + fill-in tables), `docs/REFERENCE_ANALYSIS.md`, `README.md`,
  this file.
- **Reference analysis, first entries** (task 0.4 step 18) —
  `docs/REFERENCE_ANALYSIS.md` gains three sections: the two verified
  divergences between SPEC §2.3's mount snippet and pinned IDF 6.1.0
  (`SDMMC_HOST_FLAG_1BIT` deliberately not set; slot `clk/cmd/d0` inert on
  ESP32), pinned-R2 init/retry facts (stale "no way to de-initialize" doc
  comment, `OV2640_PID = 0x26`, `width*height/5` confirmed), and the eight
  §1A.5 matrix rows verified first-hand against the pinned R1/R2 source.
- **Wi-Fi smoke module** (task 0.3 step 14) — `wifi_bringup.{h,cpp}`: APSTA
  SoftAP `visioncam-smoke` + one blocking scan (IDF 6.1 renamed the API to
  `esp_wifi_scan_start`), no credentials anywhere, nothing joined. `main.cpp`
  runs camera + SD cycles with the radio off, then on for 3 s of beacon TX.
- **Forced first-attempt failure test** (step 11 part 2) — attempt 1 runs with
  `pin_xclk = -1` so the SCCB probe genuinely fails, the normal retry
  (`cameraShutdown()` + PWDN power-cycle) must recover on attempt 2. First
  attempt with a PWDN hold **failed the test on hardware**: R2's
  `camera_probe` releases PWDN itself (`esp_camera.c:204-216`).
- **Hardware evidence logs** — `docs/evidence/` holds the raw serial captures
  behind every claim in `docs/HARDWARE.md` §2/§4/§5.

### Fixed

- **`camera_bringup.cpp` comment overstated why `cameraShutdown()` is
  required.** A *failed* `esp_camera_init` already deinitialises via its
  `fail:` label (`esp_camera.c:374-376`); the explicit `esp_camera_deinit()`
  is what the post-success exit paths (PID mismatch, warm-up, malformed
  frame) need, or a later bring-up hits `ESP_ERR_INVALID_STATE`. Comment now
  matches the pinned source.
- **Partition table silently not reaching the flashed binary.**
  `sdkconfig.defaults` set `CONFIG_PARTITION_TABLE_CUSTOM*` correctly, but
  PlatformIO's SCons pipeline reads `board_build.partitions` instead and fell
  back to the framework's `partitions_singleapp.csv`, so
  `pio run -t upload` would have flashed a 1 MB `factory` table and placed the
  app at `0x10000` — inside our NVS region. Added
  `board_build.partitions = partitions.csv`; both pipelines now produce
  byte-identical tables (md5 `7ca60f29f1fa15cf0b949d746387d5da`) and the app
  offset resolves to `0x20000`. See `docs/adr/0001-toolchain.md`.

### Notes

- **Phase 0 smoke test VERIFIED on hardware 2026-11-15** (owner's board,
  evidence in `docs/evidence/`): flash + auto-reset, `POWERON_RESET` banner,
  **PSRAM 8,388,608 B** (4 MB mapped), OV2640 PID `0x0026` + 243,471 B JPEG,
  forced-failure recovery at `attempts=2`, SD 1-bit mount + self-test at
  20,000 kHz, **GPIO4 lamp OFF for the whole boot**, Wi-Fi off/on cycles
  (`step 14 result: OK`, 2 networks heard), single boot, no
  brownout/panic/WDT. Verdict line: `camera: OK | sd: OK | psram: OK |
  wifi(step14): OK | forced-retry: OK`. Recorded in `docs/HARDWARE.md` §5.
- **D2 R1 baseline recorded** — measurements are **not** `UNVERIFIED` any more
  (`docs/HARDWARE.md` §4). The three items owed to the owner's eyes/hands
  (`owner-decisions.md` D2) were asked for in one message and are **all closed
  2026-11-16** (see the Phase 0 closure entry above): D5 answered (genuine
  SanDisk 8 GB, no class marking), GPIO4 lamp observed under R1, weak-supply
  drill skipped by owner decision. Phase 0's own phase-end
  stack-HWM/FPS row stays `UNVERIFIED` until a capture loop exists (Phase 1+).
- **Two R1 behaviours recorded as rejected** (`docs/REFERENCE_ANALYSIS.md`):
  `checkFreeStorage()`'s oldest-folder deletion loop, which on a volume
  smaller than `sdMinCardFreeSpace` can never exit (it was disarmed on the
  owner's card before any recording, in warn-only mode); and R1's AVI stats
  block, which counts bytes it *attempted* to write — on a full disk it
  reported `File size: 12.3MB` / `3796 kB/s` for a file that was really
  1,349,632 B. Phase 3's recorder must check every write's return value.
- **Hardware quirks found:** SD mount errors varied (`ESP_ERR_TIMEOUT` /
  `ESP_ERR_INVALID_RESPONSE`) until the card was re-seated — contact, not
  firmware (`docs/HARDWARE.md` §5.1); IDF maps only 4 MB of the 8 MB PSRAM;
  the ESP-IDF SDMMC driver does **not** light GPIO4 (R1's workaround is
  Arduino-`SD_MMC`-specific).
- Build sizes with Wi-Fi: RAM 15.4 % (50,584 / 327,680 B), Flash 1,041,544 B
  = 66.2 % of the 1.5 MB `ota_0` slot — Wi-Fi + mbedTLS + supplicant cost
  ≈654 KB flash over the no-radio build.
- All six `sdkconfig.defaults` symbols confirmed effective in the generated
  `sdkconfig.h`; PSRAM defaults to 40 MHz and flash to 2 MB in IDF 6.1.0, so
  both are overridden explicitly.
- Two build failures diagnosed and documented in ADR-0001: missing
  `PRIV_REQUIRES esp_psram` (renamed `main` component gets no automatic
  dependencies) and `esp_psram.c` being compiled only under `CONFIG_SPIRAM`.
- Log-level trap documented in `sdkconfig.defaults`: with
  `CONFIG_LOG_MAXIMUM_LEVEL_WARN` every `ESP_LOGI` is compiled out and
  `esp_log_level_set()` cannot restore it — the maximum must be INFO while
  the runtime default stays WARN.
