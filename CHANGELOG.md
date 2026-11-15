# Changelog

All notable changes to VisionCam Pro are recorded here.
Format follows [Keep a Changelog](https://keepachangelog.com/); versions are
tagged once Phase 9 release criteria pass.

## [Unreleased]

### Added

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
- **Still `UNVERIFIED`:** the **D2 R1 baseline** (FPS per frame size/quality,
  R1 heap figures, SD write MB/s, weak-supply brownout drill, exact SD card
  label for D5) and Phase 0's phase-end stack-HWM/FPS row — all owed to the
  owner-hardware R1 run (`docs/HARDWARE.md` §3–§4).
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
