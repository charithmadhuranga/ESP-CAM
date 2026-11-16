# Phase 0 — Repository and platform baseline (owner decisions)

> Open decisions that block Phase 0. Answer these before implementation starts.

## D1 — Licence option (blocking, SPEC §1A.2)

R1 (`s60sc/ESP32-CAM_MJPEG2SD`) is **AGPL-3.0**. Choose one and record it in
`docs/REFERENCE_ANALYSIS.md`:

- **Option A — Clean-room (default, recommended).** VisionCam Pro stays under
  your own licence. R1 is only a behavioural reference: ideas, architecture
  and measured numbers may be reused; R1 source may **not** be copied or
  mechanically translated, and is never linked into a release build.
- **Option B — Adopt AGPL-3.0.** VisionCam Pro becomes AGPL-3.0 and R1 code
  may be reused with attribution. This carries network-source-offer
  obligations for every device you ship.

R2 (`espressif/esp32-camera`, Apache-2.0) may be used under either option;
its licence notice stays in the release.

## D2 — R1 baseline run (blocking for all performance claims) — **EXECUTED 2026-11-15**

Flash R1 **unmodified** on the owner's board with its own Arduino toolchain
(arduino-esp32 ≥ 3.1.1, `CAMERA_MODEL_AI_THINKER`) and record the baseline
table in `docs/HARDWARE.md`: FPS per frame size and quality, SD write speed,
free/min heap, largest free block, GPIO4/lamp behaviour with SD mounted,
brownout/reset behaviour, exact SD card model.

Without this, every VisionCam performance number stays `UNVERIFIED`
(AGENTS rules 26 and 34).

**Run and recorded 2026-11-15** — R1 was provisioned onto the owner's LAN
over NVS (`docs/HARDWARE.md` §3.7, `tools/nvs_provision`) and measured from
this workstation while R1's own serial stats were captured. Results in
`docs/HARDWARE.md` §4 with evidence in
`docs/evidence/phase0-d2-r1-avi-stats-blocks.txt`:

| Measured | Result |
|---|---|
| FPS per frame size (quality 12) | QVGA 30.8, VGA 20.6, SVGA 20.4, XGA 5.3, SXGA 6.2 |
| FPS vs quality (VGA) | q8 20.7, q12 20.6, q16 20.4 |
| SD write speed | 493–520 kB/s (0.48–0.51 MB/s), consistent across sizes |
| Free heap while recording | 72,024–75,072 B; largest block 40,948–42,996 B; lifetime min 53,504 B; pSRAM free 914,892 B |
| Brownout/reset behaviour | none — 8 recordings + Wi-Fi TX, zero `BROWNOUT`/panic/WDT |

**All three items are now closed (2026-11-16)** — they were asked for in
one message and answered together:

1. **GPIO4 lamp under R1:** owner observed a flash in the boot window and a
   repeating blink afterwards. R1's source timeline separates the two —
   SD mounts at `00:00:00.572`, R1's lamp feature starts at
   `00:00:01.277` and forces brightness while setup is incomplete — so the
   later blink is R1's status pattern, and the early flash matches its
   `SD_MMC` pin-4 workaround comment (`utilsFS.cpp:81-84`). VisionCam's
   IDF driver leaves the lamp off for the whole boot. Recorded in
   `docs/HARDWARE.md` §4; evidence `docs/evidence/phase0-d2-r1-lamp-run.log`.
2. **Printed SD speed class:** **no class marking visible** on the card →
   D5 recorded as genuine SanDisk 8 GB, class "no marking".
3. **Weak-supply brownout drill:** **skipped by owner decision** —
   normal-supply behaviour is verified brownout-free on both builds.

**Storage surprise — RESOLVED 2026-11-16:** the card's CSD reports
8,053,063,680 B and its CID calls it `SD16G`, but both FATFS implementations
reported a **5,216,256 B FAT12 volume with 1,349,632 B free**. The owner
then identified the card (genuine **SanDisk 8 GB**, pre-flashed with a
bootable **Linux Mint** image — the 5 MB FAT was its EFI partition, the rest
ext4) and authorized a rewrite. `tools/sd_format/` reformatted it to
MBR + FAT32 across the whole card (**8,051,032,064 B usable, 99 % of CSD**),
verified by `docs/evidence/phase0-sd-format.log` and VisionCam boot 12.
Full analysis of the original finding (including why one R1 stats block
reported 12.3 MB / 3796 kB/s for a 1.3 MB truncated file) is
`docs/HARDWARE.md` §4.2/§4.2.1.

## D3 — FPS parity tolerance

How close must VisionCam's recording FPS match the R1 baseline at the same
frame size, quality and card? Suggested: **10 %**.

**Answered: 10 % (2026-11-15)** — acceptance bar for the Phase 3 and
Phase 9 parity tests (SPEC §11.10).

## D4 — On-device TFLite Micro (needed by Phase 6)

The original ESP32 has little headroom for TFLM. After the Phase 6 benchmark
you may declare on-device AI **unsupported** beyond the single benchmarked
model. Confirm you accept that outcome.

## D5 — Deployment SD card (needed by Phase 9)

Which SD card model(s) will the 24-hour long-run and power-loss tests use?
Record the exact model — card quality is a measured variable (R1 found a
no-name "Class 6" card about 3x slower than a genuine Sandisk 4 GB Class 2).

---

**Answered:**

| # | Decision | Answer | Date |
|---|---|---|---|
| D1 | Licence A / B | **A — Clean-room.** R1 is behavioural reference only; no R1 code copied or mechanically translated; never linked into a release build. | 2026-11-15 |
| D2 | R1 baseline run | **Run during Phase 0.** Owner flashes R1 unmodified and records the baseline table; procedure is Phase 0 task 1. | 2026-11-15 |
| D3 | FPS tolerance | **10 %.** Acceptance bar for Phase 3 / Phase 9 parity tests (SPEC §11.10). | 2026-11-15 |
| D4 | TFLM supported / unsupported | *(pending)* | |
| D5 | SD card model | **Answered: genuine SanDisk 8 GB, no speed-class marking visible** (owner 2026-11-16). Was pre-flashed with Linux Mint; reformatted MBR+FAT32 by `tools/sd_format/` same day (`docs/HARDWARE.md` §4.2.1). Class recorded as "no marking" — Phase 9 measures real throughput instead | 2026-11-16 |
