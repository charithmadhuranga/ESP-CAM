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

## D2 — R1 baseline run (blocking for all performance claims)

Flash R1 **unmodified** on the owner's board with its own Arduino toolchain
(arduino-esp32 ≥ 3.1.1, `CAMERA_MODEL_AI_THINKER`) and record the baseline
table in `docs/HARDWARE.md`: FPS per frame size and quality, SD write speed,
free/min heap, largest free block, GPIO4/lamp behaviour with SD mounted,
brownout/reset behaviour, exact SD card model.

Without this, every VisionCam performance number stays `UNVERIFIED`
(AGENTS rules 26 and 34).

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
| D5 | SD card model | *(pending — needed by Phase 9)* | |
