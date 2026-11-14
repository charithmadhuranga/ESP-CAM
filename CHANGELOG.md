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
- **Docs** — `docs/adr/0001-toolchain.md`, `docs/HARDWARE.md` (R1 baseline
  procedure + fill-in tables), `docs/REFERENCE_ANALYSIS.md`, `README.md`,
  this file.

### Notes

- First `pio run` succeeded: RAM 4.3 %, Flash 17.5 %, `firmware.bin` 184,208 B.
- All six `sdkconfig.defaults` symbols confirmed effective in the generated
  `sdkconfig.h`; PSRAM defaults to 40 MHz and flash to 2 MB in IDF 6.1.0, so
  both are overridden explicitly.
- Two build failures diagnosed and documented in ADR-0001: missing
  `PRIV_REQUIRES esp_psram` (renamed `main` component gets no automatic
  dependencies) and `esp_psram.c` being compiled only under `CONFIG_SPIRAM`.
- **Nothing is hardware-verified yet.** No serial device is attached; every
  flashing, camera, SD and PSRAM-on-device claim remains `UNVERIFIED`.
