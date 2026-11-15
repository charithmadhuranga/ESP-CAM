# Phase 0 — Repository and platform baseline

**Goal:** a reproducible, pinned build that flashes the AI-Thinker board,
proves camera + PSRAM + SD work together, and records the hardware baseline
every later phase is measured against.

**Spec:** §1A (esp. 1A.2, 1A.7, 1A.10), §2, §3.3.1, §12 Phase 0, §13, §18.
**Skills:** `visioncam-reference-first`, `visioncam-build-flash`,
`visioncam-hardware-validation`.

---

## Entry criteria

- Owner has answered **D1** (licence option) and arranged board access for
  **D2** (R1 baseline run).
- `SPEC.md`, `DESIGN.md`, `mockup-ui/`, `reference/` present (they are).

## Deliverables

1. PlatformIO project with `framework = espidf`, pinned platform + toolchain.
2. Pinned `espressif/esp32-camera` IDF component (exact version/commit).
3. AI-Thinker board profile header with the §2.2 pin map (single source).
4. `sdkconfig.defaults` baseline: PSRAM on, flash/PSRAM 80 MHz, only OV2640
   enabled, explicit camera task core, DMA/JPEG buffer settings, log levels.
5. `partitions.csv` sized for firmware + embedded web assets (+ OTA slots if
   chosen), with headroom recorded.
6. SD 1-bit mount test, PSRAM detection report, serial diagnostics.
7. `reference/README.md` (AGPL warning), `reference/fetch_references.sh`,
   `reference/pins.txt`.
8. `docs/` skeleton: `HARDWARE.md`, `ARCHITECTURE.md`, `REFERENCE_ANALYSIS.md`,
   `adr/0001-toolchain.md`.
9. `README.md`, `CHANGELOG.md`, `.gitignore`, git repository with the spec
   baseline committed.
10. Hardware smoke-test guide with a fill-in baseline table.

## Ordered tasks

### 0.1 Repository

1. `git init`; `.gitignore` covering `.pio/`, `node_modules/`, `dist/`,
   `*.pyc`, `__pycache__/`, OS junk. **Do not ignore `reference/`** — it is
   pinned evidence — but note in `reference/README.md` that it is excluded
   from the firmware build and from release archives.
2. Commit `SPEC.md`, `DESIGN.md`, `mockup-ui/`, `reference/` as the baseline.
3. Create `docs/`, `docs/adr/`, `firmware/`, `webui/`, `tools/`, `models/`,
   `test/` per SPEC §13.
4. Write `reference/README.md` (R1 is AGPL-3.0, never vendored, clean-room
   rules), `reference/pins.txt` with the two pinned commits, and
   `reference/fetch_references.sh` that clones/checkout-verifies them.

### 0.2 Toolchain and ADR

5. Choose and pin: PlatformIO `platform` version, the ESP-IDF version it
   provides, `framework = espidf`, board definition (start from PlatformIO's
   AI-Thinker ESP32-CAM definition and **verify flash size, partition table
   and PSRAM settings against the physical module**).
6. Write `docs/adr/0001-toolchain.md`: platform version, IDF version,
   `sdkconfig.defaults` baseline, partition table choice, why.
7. Add R2 as an ESP-IDF component pinned in `firmware/src/idf_component.yml`
   with an exact version. Add a build-time check that only one copy of
   `esp32-camera` is linked (AGENTS rule 33).
8. Add `esp-mqtt` pinned now too (needed by Phase 7) so the dependency set is
   stable early.

### 0.3 Board profile and smoke firmware

9. `firmware/include/board_profile.h`: the §2.2 AI-Thinker pin map and
   nothing else. No pin constant may appear anywhere else (AGENTS rule 6).
10. Minimal firmware that: initialises logging, reports
    `esp_reset_reason()`, detects PSRAM (`esp_psram_get_size()` — verify the
    symbol for the pinned IDF), reports free heap / largest free block /
    minimum heap, then prints a banner. Flash and confirm on serial.
11. Camera bring-up per SPEC §4.1.1: init at the largest frame size the PSRAM
    holds, read sensor PID and require OV2640, retry twice with a PWDN
    (GPIO32) power-cycle between attempts, discard ≥3 warm-up frames, grab
    one JPEG and print its length. Fail with a readable message otherwise.
12. SD mount per SPEC §2.3: `esp_vfs_fat_sdmmc_mount` at `/sdcard`,
    `SDMMC_HOST_FLAG_1BIT`, `slot.width = 1`,
    `format_if_mount_failed = false`, `max_files = 5`. Write + read + delete
    a test file.
13. **GPIO4 test:** confirm whether the ESP-IDF SDMMC 1-bit driver lights the
    lamp; if it does, drive GPIO4 low after mount and record the result in
    `docs/HARDWARE.md` (R1's workaround was written for Arduino `SD_MMC`).
14. Camera + SD running together under Wi-Fi off, then Wi-Fi on — confirm no
    instability, no brownout.

### 0.4 R1 baseline (owner hardware, **D2**)

15. Flash R1 **unmodified** with its own Arduino toolchain
    (arduino-esp32 core ≥3.1.1, `CAMERA_MODEL_AI_THINKER`, Minimal SPIFFS
    partition). This is a measurement tool only — not VisionCam's build
    system.
16. Fill the baseline table in `docs/HARDWARE.md`: recorded FPS per frame
    size/quality, SD write speed, free heap, minimum heap, largest free
    block, GPIO4/lamp behaviour with SD mounted, brownout/reset behaviour,
    exact SD card model.
17. Record the tolerance (**D3**, suggested 10 %) as the acceptance bar for
    Phase 3 and Phase 9 parity tests (SPEC §11.10).
    **Recorded: 10 % (2026-11-15)** — `owner-decisions.md`, `docs/HARDWARE.md`
    §1 and §4.
18. Write the first `docs/REFERENCE_ANALYSIS.md` entries: the §1A.5 matrix
    rows you have now verified first-hand, plus any behavioural difference
    found between the pinned R1/R2 and what SPEC assumed.

## Memory / timing / hardware notes

- PSRAM size decides the init frame size (≈2 MB → SVGA, ≈4 MB → UXGA).
- Do not raise XCLK above 20 MHz without measurement.
- Keep DRAM free for stacks, DMA and network buffers; frames go to PSRAM.
- Never write to SD from an ISR; no auto-format, ever.

## Tests and acceptance

- [x] `pio run` produces a firmware image with no warnings-as-errors ignored.
- [x] Firmware flashes using ESP32-CAM-MB (auto-reset, no BOOT press).
- [x] Serial log shows reset reason, PSRAM size, heap figures.
- [x] Camera captures a valid JPEG; sensor PID reported as OV2640.
- [x] Forced first-attempt camera failure → retry + PWDN power-cycle recovers
      (`attempts=2 -> OK`; sabotage = `pin_xclk = -1`, because R2 releases
      PWDN itself — evidence `docs/evidence/phase0-smoke-boot-11-*.log`).
- [x] SD mounts in 1-bit mode; test file round-trips.
- [x] GPIO4/lamp behaviour with SD mounted documented (**OFF** for the whole
      boot — IDF SDMMC driver does not light it).
- [x] Camera and SD operate together; stable power under Wi-Fi TX + camera
      (`step 14 result: OK`, 2 networks heard, 3 s beacon TX).
- [x] Brownout/reset behaviour recorded — **this run**: 1 boot,
      `POWERON_RESET`, none observed. Weak-supply drill still owed with D2.
- [ ] R1 baseline table complete, or explicitly marked `UNVERIFIED` —
      `docs/HARDWARE.md` §4 rows still `UNVERIFIED`; **owed to the owner's
      D2 run** (R1 flash + measurements, §3.3–3.6).
- [x] Only one `esp32-camera` copy linked (rule 33 check passes at configure).
- [x] `docs/adr/0001-toolchain.md` and `docs/REFERENCE_ANALYSIS.md` exist.

## Exit gate

Flashes, captures JPEG, mounts SD, PSRAM reported, baseline recorded, licence
option (D1) written into `docs/REFERENCE_ANALYSIS.md`.

## Docs updated

`README.md`, `CHANGELOG.md`, `docs/HARDWARE.md`, `docs/ARCHITECTURE.md`
(initial), `docs/REFERENCE_ANALYSIS.md`, `docs/adr/0001-toolchain.md`.

## Risks / known gaps

- PlatformIO AI-Thinker board definition may disagree with the physical
  module on flash size or PSRAM — verify, do not trust.
- R2 `library.json` says 2.0.0 while the registry says 2.1.9; do not use a
  floating entry.
- If the board cannot be reached, every hardware line above stays
  `UNVERIFIED` and Phase 1 must not begin.
