---
name: VisionCam hardware validation and measurement
description: Bench-test and measure VisionCam Pro on the real AI-Thinker ESP32-CAM — serial monitoring, resource metrics (heap, largest block, stack watermarks, FPS, SD write speed), the Phase 0 R1 baseline procedure, the 24-hour mixed-workload and power-loss test routines, degraded-mode drills, and how to record results honestly in docs/TEST_PLAN.md. Load whenever a change has hardware behaviour, a performance claim is being made, or a phase exit gate needs proving.
license: MIT
metadata:
  version: "1.0"
  author: visioncam-pro
---

# Hardware validation skill

Read `AGENTS.md` (especially rules 22 and 26), the active phase file, and
`.opencode/plans/11-acceptance-matrix.md` first.

## The honesty rules

- **Never claim a test passed unless it was actually run.** Use `RUN PASS`,
  `RUN FAIL`, or `UNVERIFIED`.
- Every performance claim needs its matching Phase 0 baseline row.
- Any shortfall against the R1 baseline is explained in
  `docs/REFERENCE_ANALYSIS.md`, not hidden.
- Hardware behaviour not yet exercised is reported as `UNVERIFIED` in the
  change summary, not as "works".

## Bench setup

- AI-Thinker ESP32-CAM + OV2640, flashed over ESP32-CAM-MB.
- Stable regulated 5 V supply with real transient headroom (R1's own finding:
  crash loops are usually the power supply, not the firmware).
- Known microSD card — record the **exact model**; a no-name "Class 6" card
  measured ~3x slower than a genuine Sandisk 4 GB Class 2 in R1's tests.
- 2.4 GHz AP, a phone for mobile checks, and a broker/HA instance for
  Phase 7.
- Serial: `pio device monitor` at 115200.

## Metrics to capture at the end of every phase

Record in `docs/ARCHITECTURE.md`:

| Group | Values |
|---|---|
| Memory | free heap, **minimum** free heap, largest free block, PSRAM total/free |
| Tasks | stack high-water mark per task, task count, priorities |
| Camera | capture rate, stream FPS, dropped/invalid frame count |
| Storage | SD write duration, write failures, recording queue depth |
| AI | inference duration per model, peak memory, tensor arena |
| MQTT | publish queue depth, reconnect count |
| System | reset reason, uptime, brownout count |

Warning thresholds to wire in: free heap < ~32 KB, largest free block
< ~16 KB, TLS session needs ~64 KB free (R1 figures — re-measure).

## Phase 0 baseline procedure (R1 as the measurement bar)

1. Flash R1 **unmodified** with its own Arduino toolchain (arduino-esp32
   ≥ 3.1.1, `CAMERA_MODEL_AI_THINKER`, Minimal SPIFFS partition). Measurement
   tool only — never VisionCam's build system.
2. For each frame size (96x96, QVGA, CIF, VGA, SVGA, XGA, HD, SXGA, UXGA) at
   max JPEG quality and 20 MHz clock, record: camera max fps, recorded fps,
   motion detection time (ms).
3. Record SD write speed, free heap, minimum free heap, largest free block.
4. Record GPIO4/lamp behaviour with the SD mounted.
5. Record brownout/reset behaviour and the exact SD card model.
6. Fill `docs/HARDWARE.md`; agree the tolerance with the owner (suggested
   10 %) and record it.

R1's published reference table (targets, not guarantees):

| Frame size | Camera max fps | R1 recorded fps | Motion detect (ms) |
|---|---|---|---|
| 96x96 | 50 | 45 | 15 |
| QVGA | 50 | 40 | 70 |
| CIF | 50 | 40 | 110 |
| VGA | 25 | 20 | 80 |
| SVGA | 25 | 20 | 120 |
| XGA | 12.5 | 5 | 180 |
| HD | 12.5 | 5 | 220 |
| SXGA | 12.5 | 5 | 300 |
| UXGA | 12.5 | 5 | 450 |

## Test routines

### Camera bring-up
- Confirm sensor PID = OV2640; force a first-attempt failure and verify the
  PWDN power-cycle retry recovers.
- Discard ≥3 warm-up frames; confirm auto-exposure settles.
- Inject/observe a NULL frame and verify recovery is rate-limited (≤1 per
  2 s, ≤10 attempts) and does **not** trip the task watchdog.

### SD / storage
- 1-bit mount, test file round-trip, camera + SD together.
- GPIO4/lamp with SD mounted — document whether the IDF driver needs
  R1's low-drive workaround.
- Full-card drill: retention runs; if still full, recording stops safely
  with a critical warning — no silent corruption, no auto-format.

### Recording
- Segment closes at the configured duration and frame cap.
- Validate AVI externally (`ffprobe`/VLC) — actually run it.
- Header FPS vs measured FPS within 5 %.
- **Power-cut test:** kill power mid-recording, reboot, confirm the chunk
  scanner truncates, rebuilds `idx1` + header, renames with the `recovered`
  flag, and the file plays. Repeat several times. Never auto-delete an
  unrecovered segment.
- Live-view FPS with recording on vs off; playback must never stall a
  recording (or must return `503 PLAYBACK_BUSY`).

### Degraded-mode drills (SPEC §9 — exercise every row)
Wi-Fi unavailable · MQTT unavailable · external AI unavailable · SD missing ·
SD full · SD write error · invalid TFLM model · AI over budget · time not
synchronised · browser disconnected · power loss · network storage
unavailable · authentication failure. In each case confirm the core camera
keeps working and the UI banner names the failed subsystem.

### 24-hour mixed workload (release gate)
Live streaming + event recording + snapshots + motion + MQTT reconnects +
storage usage + timelapse, for ≥24 h. Track heap, min heap, largest block,
stack watermarks, dropped frames, SD errors and reset count. Accept: no
unexplained resets, no unbounded memory growth. Then repeat
reboot/power-interruption recovery tests on the exact deployment card.

## Troubleshooting order (crash / instability)

Reset reason + serial logs → heap/min-heap/largest block/PSRAM → stack
watermarks → disable AI → disable network storage → disable continuous
recording → camera alone → SD alone → camera + SD → re-enable one subsystem
at a time.

Never add arbitrary delays to "stabilise" a crash without identifying the
resource or synchronisation issue. Suspect the power supply first.

## Recording results

Maintain `docs/TEST_PLAN.md` with one row per item from
`.opencode/plans/11-acceptance-matrix.md`: item, phase, procedure, result
(`RUN PASS` / `RUN FAIL` / `UNVERIFIED`), date, hardware (board + card
model), and notes. Update the matrix status column to match.
