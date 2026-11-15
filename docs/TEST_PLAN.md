# Test plan and live status

The **map** (which acceptance item is proved by which phase and skill) lives
in `.opencode/plans/11-acceptance-matrix.md`. This file carries the **live
status**. Status values: `TODO` → `RUN PASS` / `RUN FAIL` / `UNVERIFIED`
(hardware or environment not yet available).

Rule: never write `RUN PASS` without a serial log or tool output behind it
(AGENTS rule 26). Evidence for hardware runs is committed under
`docs/evidence/`.

---

## Phase 0 — repository and platform baseline

Hardware run: **2026-11-15**, owner's AI-Thinker ESP32-CAM over
ESP32-CAM-MB. Evidence:
`docs/evidence/phase0-smoke-boot-10-wifi-lamp.log`,
`docs/evidence/phase0-smoke-boot-11-forced-retry.log`.

| # | Test | Status | Evidence |
|---|---|---|---|
| 0.1 | Build: `pio run` green, pinned toolchain | RUN PASS | `docs/adr/0001-toolchain.md` verification table |
| 0.2 | Single `esp32-camera` copy (rule 33) | RUN PASS | configure-time check, pass + fail path exercised |
| 0.3 | Flash via ESP32-CAM-MB (auto-reset) | RUN PASS | boot 11 log, `POWERON_RESET` |
| 0.4 | Banner: reset reason / PSRAM / heap | RUN PASS | boot 11 log — `PSRAM: 8388608 bytes`, heap free 4,448,703 B |
| 0.5 | Camera: PID OV2640, ≥3 warm-ups, JPEG captured | RUN PASS | boot 11 log — `PID 0x0026`, 243,471 B (1600×1200) |
| 0.6 | Forced first-attempt failure → retry recovers | RUN PASS | boot 11 log — `attempts=2 -> OK` (sabotage `pin_xclk=-1`) |
| 0.7 | SD mounts 1-bit, write/read/delete round-trip | RUN PASS | boots 8–11 — `SSR: bus_width=1`, self-test OK |
| 0.8 | GPIO4/lamp with SD mounted | RUN PASS | owner observation: **OFF** the whole boot (2026-11-15) |
| 0.9 | Camera + SD together, Wi-Fi off then on, no brownout | RUN PASS | boots 10/11 — `step 14 result: OK`, 1 boot, no `BROWNOUT` |
| 0.10 | Partition table as flashed | RUN PASS | boot 11 log lists all 6 partitions from `partitions.csv` |
| 0.11 | **D2 — R1 baseline table** (`docs/HARDWARE.md` §4: FPS per size/quality, R1 heap, SD write MB/s, weak-supply brownout, exact card label → D5) | **UNVERIFIED** | needs the owner to flash R1 and take measurements (`docs/HARDWARE.md` §3.3–3.6) |
| 0.12 | Phase-end resource row: stack high-water marks, FPS | **UNVERIFIED** | needs R1 baseline + a running VisionCam capture loop (Phase 1+) |

## Known hardware quirks found in Phase 0

- **SD contact:** mount errors varied between boots (`ESP_ERR_TIMEOUT` /
  `ESP_ERR_INVALID_RESPONSE`) until the card was re-seated — six of the first
  seven boots failed; boots 8–11 all passed. Re-seat before debugging code
  (`docs/HARDWARE.md` §5.1).
- **PSRAM:** 8 MB fitted, only **4 MB mapped** by IDF on the original ESP32.
- **GPIO4:** ESP-IDF SDMMC 1-bit driver does **not** light the lamp (Arduino
  `SD_MMC` does — that is what R1's workaround targets).
- **R2 PWDN:** `esp_camera.c:204-216` releases PWDN at the start of *every*
  init, so an external PWDN hold cannot force a failed attempt.

## Later phases

Not started. Every row stays `TODO` until its phase runs; see
`.opencode/plans/11-acceptance-matrix.md` for the full map.
