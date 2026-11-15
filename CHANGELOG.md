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
- **Owner decisions D1, D2** recorded (clean-room licence; R1 baseline run
  during Phase 0).
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

### Fixed

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

- Phase 0 build sizes (host-side): RAM 8.2 % (26,840 / 327,680 B),
  Flash 387,356 B = 24.6 % of the 1.5 MB `ota_0` slot;
  `firmware.bin` 387,792 B.
- Everything hardware-dependent — flashing, camera, SD, PSRAM-on-device,
  GPIO4 lamp, brownout — is **UNVERIFIED**: no serial device is attached.
- All six `sdkconfig.defaults` symbols confirmed effective in the generated
  `sdkconfig.h`; PSRAM defaults to 40 MHz and flash to 2 MB in IDF 6.1.0, so
  both are overridden explicitly.
- Two build failures diagnosed and documented in ADR-0001: missing
  `PRIV_REQUIRES esp_psram` (renamed `main` component gets no automatic
  dependencies) and `esp_psram.c` being compiled only under `CONFIG_SPIRAM`.
- **Nothing is hardware-verified yet.** No serial device is attached; every
  flashing, camera, SD and PSRAM-on-device claim remains `UNVERIFIED`.
