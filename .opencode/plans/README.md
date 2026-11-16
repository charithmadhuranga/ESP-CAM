# VisionCam Pro — Implementation Plan

Phase-by-phase work orders for building VisionCam Pro from the current
documentation-only repository to a versioned firmware release.

**Start here:** `00-analysis.md`, then `owner-decisions.md`, then the phase
file for the phase you are working on, then the acceptance matrix in
`11-acceptance-matrix.md`.

---

## Current status (2026-11-16)

**Phase 0 — code and hardware complete; **D2 R1 baseline MEASURED** 2026-11-15; **SD card reformatted** 2026-11-16.**

- ✅ Done: tasks 0.1–0.4 (repo baseline, pinned toolchain + components,
  board profile, partition table verified on both build pipelines **and in
  the boot log on hardware**, camera + SD + Wi-Fi smoke modules),
  task 0.4 step 18 (`docs/REFERENCE_ANALYSIS.md` entries), R1 compile check,
  D3 answered (**10 %**).
- ✅ **Smoke test VERIFIED on the owner's board 2026-11-15** — every §5 row in
  `docs/HARDWARE.md`: flash/auto-reset, banner + PSRAM 8,388,608 B, OV2640
  PID + JPEG, forced-failure `attempts=2 -> OK`, SD 1-bit mount + self-test,
  **GPIO4 lamp OFF**, Wi-Fi off/on cycles OK, single boot, no brownout.
  Evidence: `docs/evidence/phase0-smoke-boot-10/11-*.log`.
- ✅ **D2 R1 baseline MEASURED** — R1 flashed unmodified, provisioned onto the
  owner's LAN over NVS (`tools/nvs_provision`), measured over HTTP while a
  passive serial capture recorded R1's own stats block. `docs/HARDWARE.md` §4
  now holds 8 runs: 30.8 / 20.6 / 20.4 / 5.3 / 6.2 FPS for QVGA…SXGA at
  quality 12 (plus VGA at q8 and q16), SD 0.48–0.51 MB/s, heap and pSRAM
  figures, no brownout/panic/WDT. Evidence:
  `docs/evidence/phase0-d2-r1-avi-stats-blocks.txt`,
  `docs/evidence/phase0-d2-r1-serial-run.txt`.
- ✅ **Storage finding RESOLVED 2026-11-16:** the card held a bootable
  **Linux Mint** image — the "5 MB FAT volume" was its EFI partition, the
  rest ext4 (invisible to FATFS). Owner authorized the rewrite; the new
  standalone `tools/sd_format/` tool (flash → `format` → typed `ERASE` →
  reflash) wrote **MBR + FAT32 across the whole card: 8,051,032,064 B
  usable, 99 % of the CSD**. Post-format VisionCam boot 12: no coverage
  warning, full verdict line green. Evidence:
  `docs/evidence/phase0-sd-format.log`,
  `docs/evidence/phase0-smoke-boot-12-postformat.log`; analysis:
  `docs/HARDWARE.md` §4.2/§4.2.1, TEST_PLAN row 0.13. Phase 9 retention
  sizing now has a card with room; R1's stats-block lesson from the
  full-disk era stands (Phase 3 must check write return values).
- ⬜ Owed to the owner, asked in **one** message: the **GPIO4 lamp check
  while R1** records (R1 claims Arduino `SD_MMC` lights GPIO4; the IDF
  driver provably does not), the **printed SD speed class** (brand and
  8 GB capacity already confirmed → D5 mostly closed), and the optional
  **weak-supply brownout drill**. Phase 0's own phase-end stack-HWM/FPS
  row needs a running capture loop — **Phase 1+**.
- Phase 0 exit gate is **green** (baseline recorded, storage resolved).
  Start Phase 1; the remaining card-level questions are owner follow-ups,
  not firmware blockers.

---

## How to use this plan

1. **One phase at a time.** A phase is not done until its *Exit gate* is
   green. Never start phase N+1 while phase N has a failing gate.
2. **Every phase must compile, flash and have a reproducible test
   procedure** before the next begins (SPEC §12).
3. **Read first, in this order:** `AGENTS.md` → the phase file → the
   relevant section of `SPEC.md` → `DESIGN.md` if the phase touches UI →
   the mapped R1/R2 code via the `visioncam-reference-first` skill.
4. **Record every decision.** Adopt/adapt/defer/reject goes in
   `docs/REFERENCE_ANALYSIS.md`; architecture choices go in an ADR under
   `docs/adr/`.
5. **Never claim a test passed unless it was actually run** (AGENTS rule 26).
   Hardware-only tests are marked `UNVERIFIED` until run on the board.
6. **Owner gates** (section below) must be answered before the phase that
   needs them.

## File map

| File | Contents |
|---|---|
| `00-analysis.md` | Inventory, constraints, supersessions, mockup/reference analysis, gaps, risks, traceability |
| `owner-decisions.md` | D1–D5 owner questions with a fill-in answer table (licence, R1 baseline, FPS tolerance, TFLM, SD card) |
| `01-phase-0-baseline.md` | Repo, platform, board profile, R1 baseline, camera + SD smoke test |
| `02-phase-1-camera-webui.md` | Camera manager, frame manager, Wi-Fi, HTTP, auth, embedded React UI, MJPEG, snapshot |
| `03-phase-2-storage-snapshots.md` | Storage manager, directory layout, JSONL indexes, gallery, retention |
| `04-phase-3-recording-playback.md` | AVI/MJPEG writer, segments, crash recovery, server-side playback, browser |
| `05-phase-4-motion-events.md` | Motion engine, event bus, event recording, UI indicators |
| `06-phase-5-timelapse.md` | Direct-to-AVI timelapse, projects, optional generator |
| `07-phase-6-ai.md` | Provider abstraction, TFLM experimental, external AI, model lifecycle |
| `08-phase-7-mqtt-ha.md` | `esp-mqtt` worker, topics, HA discovery, reconnect |
| `09-phase-8-network-storage.md` | `IStorageBackend`, HTTP upload queue, integration UI |
| `10-phase-9-hardening-release.md` | Long-run, security review, power-loss recovery, migration, release docs |
| `11-acceptance-matrix.md` | SPEC §11 + §11.10 test matrix mapped to phases and skills |

## Phase gates

| Phase | Name | Hard gate before proceeding |
|---|---|---|
| 0 | Repository and platform baseline | Flashes via ESP32-CAM-MB; JPEG capture; SD mounts 1-bit; PSRAM detected; R1 baseline table recorded; licence option decided |
| 1 | Core camera and web UI | Authenticated browser access; live view on phone + laptop; no unauthenticated stream; UI assets load with no internet |
| 2 | Storage and snapshots | Snapshots survive reboot; gallery paginates; index rebuilds |
| 3 | Recording and playback | AVI validates in desktop tools; interrupted segment recovers; live view stays responsive during recording |
| 4 | Motion and event engine | Configurable trigger drives snapshot + recording; bounded event queue; tunable false-trigger rate |
| 5 | Timelapse | Short project captured and produced without disrupting the camera |
| 6 | AI framework | Invalid model rejected; one benchmarked model runs **or** on-device AI declared unsupported; external AI failure does not stop the camera |
| 7 | MQTT and Home Assistant | Entities appear in HA; events publish; broker outage does not interrupt recording |
| 8 | Network storage | Files upload after segment close; network failure preserves local files; queue bounded |
| 9 | Hardening and release | 24 h mixed workload; no unexplained resets; no unbounded heap growth; media recovery verified; all optional integrations fail gracefully |

## Owner decisions required (blockers)

Full write-up and answer table: [`owner-decisions.md`](owner-decisions.md).

| # | Decision | Needed by | Status |
|---|---|---|---|
| D1 | Licence: **A clean-room** or **B adopt AGPL-3.0** | Phase 0 | ✅ **A — clean-room** (2026-11-15): no R1 code may be copied |
| D2 | Run R1 unmodified on the board and record the baseline table | Phase 0 | ✅ **Run during Phase 0** (2026-11-15); performance claims stay `UNVERIFIED` until numbers are recorded |
| D3 | FPS parity tolerance vs R1 baseline | Phase 0 | ✅ **10 %** (2026-11-15): VisionCam recording FPS must stay within 10 % of the R1 baseline at the same frame size, quality and card |
| D4 | Declare on-device TFLM supported or unsupported after benchmark | Phase 6 | ⬜ pending — default: unsupported beyond the benchmarked model |
| D5 | Deployment SD card model(s) for the long-run test | Phase 9 | 🟡 **partially answered** — genuine **SanDisk 8 GB** (owner 2026-11-16; was pre-flashed with Linux Mint, reformatted MBR+FAT32 same day). Still owed: the printed speed class |

## Global engineering rules for every phase

- Single frame owner, single filesystem owner, bounded queues, versioned
  contracts (SPEC §3.2).
- Optional subsystems fail soft; core camera keeps running (SPEC §9).
- Measure heap, largest free block, stack high-water marks and frame rates
  at the end of every phase and record them in `docs/ARCHITECTURE.md`.
- Update `CHANGELOG.md` and the relevant `docs/*` whenever behaviour or a
  contract changes (AGENTS rules 28, 20).
- Smallest vertical slice first; compile and test it before widening.
