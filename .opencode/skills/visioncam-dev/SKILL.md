---
name: VisionCam Pro Embedded Camera Development
description: Main workflow skill for building VisionCam Pro — the ESP32-CAM (original ESP32, OV2640) firmware in PlatformIO/ESP-IDF plus its embedded React UI. Load when implementing, reviewing or debugging any VisionCam subsystem: camera/frame handling, storage, AVI recording, motion, timelapse, AI providers, MQTT/Home Assistant, the web UI, or when troubleshooting a crash, heap exhaustion or brownout on the device.
license: MIT
metadata:
  version: "1.0"
  author: visioncam-pro
---

# VisionCam Pro development skill

Canonical `SKILL.md` for this repository (SPEC §15). Paths are relative to
the repository root.

## Read before any change

1. `AGENTS.md` — always (37 mandatory rules).
2. `.opencode/plans/README.md` — which phase is active and its exit gate.
3. The active `.opencode/plans/0N-phase-*.md` work order.
4. `SPEC.md` sections named by that phase file.
5. `DESIGN.md` when the change touches UI.
6. `docs/REFERENCE_ANALYSIS.md` before designing a subsystem — and append
   your adopt/adapt/defer/reject decision to it.
7. Relevant `firmware/AGENTS.md` or `webui/AGENTS.md`.

Only load what the focused change needs, but always load `AGENTS.md`.

## Hardware profile (never substitute ESP32-S3 assumptions)

- Board: AI-Thinker ESP32-CAM · MCU: original ESP32 · Camera: OV2640
- Programmer: ESP32-CAM-MB · Wi-Fi: 2.4 GHz only
- SD: onboard microSD, ESP-IDF SDMMC host, **1-bit**, never auto-format
- Flash LED: GPIO4, shared with SD — hardware constraint
- Build: PlatformIO `framework = espidf` (no Arduino layer)
- Pins: only from `firmware/include/board_profile.h`

## Development procedure for every change

1. Identify the subsystem and read its interface + current tests.
2. Identify all producers/consumers of the shared resource it touches.
3. Check stack, heap, PSRAM, flash and timing impact — with numbers.
4. Define bounded queues and explicit timeouts.
5. Implement behind a stable interface, smallest vertical slice first.
6. Add error and degraded-mode handling.
7. Add tests or a documented hardware validation procedure.
8. Build firmware **and** web UI; run the tests.
9. Update the relevant doc + `CHANGELOG.md`.
10. Report completed work and remaining limitations honestly. Mark anything
    not run on hardware as `UNVERIFIED`.

## Subsystem rules

### Camera / frame handling
- Use the official `espressif/esp32-camera` component, pinned, single copy.
- `frame/` is the only module that acquires or releases camera buffers.
- Release every frame exactly once; never hold a camera frame across
  blocking I/O; copy into a bounded PSRAM buffer for slow consumers.
- Init once at the largest frame size PSRAM allows, then `set_framesize`
  at runtime; `fb_count = 2`, `CAMERA_GRAB_LATEST`, XCLK 20 MHz.
- Read the sensor PID and require OV2640; retry twice with a PWDN
  power-cycle; discard ≥3 warm-up frames.
- Drop and count frames with `len == 0` or `len` > buffer size.
- NULL frame: yield ~10 ms, recover at most once per 2 s for at most 10
  attempts, then raise `CameraFailed` and stop consumers cleanly. Never spin.
- Limit concurrent streams (default 1 on the original ESP32); measure
  dropped frames and capture rate.

### Storage
- All filesystem operations go through `storage/` — nothing else opens files.
- Opaque IDs in every API; validate against path traversal.
- Append-only JSONL indexes, independently validated per line, rebuildable
  from the directory scan; filesystem is source of truth.
- Write to a temporary name, finalise, then rename; `fsync` where it matters.
- Stage writes in sector-multiple (8 KB) blocks; confirm on the card.
- Handle full card and write failure; never delete open files; never
  auto-format; never delete an unrecovered segment.

### Recording
- JPEG/MJPEG AVI only — never fake MP4/H.264.
- Derive the RIFF/AVI structure from the public format, never from R1's byte
  table (licence rule).
- Short segments (default 60 s), provisional header at start, boot-time
  `00dc` chunk-scan recovery, `recovered` flag in the index.
- Header stores **measured** FPS, not requested FPS.
- One writer task; mutex only around header patching.
- Server-side timed MJPEG replay is the primary playback path; playback must
  never stall a recording, otherwise `503 PLAYBACK_BUSY`.

### Motion
- Decode JPEG to a very small grayscale bitmap — the downscale is the noise
  filter; no full-resolution RGB buffers.
- Two configurable sample rates: ~5/s idle, 1/2 s while recording.
- Not offered above SXGA; the UI must say why.
- Same decode yields ambient light with hysteresis; provide a debug overlay.
- Trigger-source bitmask recorded on every event.
- Motion keeps running during continuous recording (divergence from R1).
- Label it clearly: motion detection is not object recognition.

### AI
- Provider abstraction: `NoAiProvider` (default), `ExternalAiProvider`
  (primary), `TfliteMicroProvider` (experimental).
- Validate operators, shapes, quantisation and tensor arena; stage uploads
  with a size cap; keep last-known-good; rollback must always work.
- Fixed/bounded tensor arena; AI task below streaming and storage priority;
  never block streaming or storage on inference.
- External AI: timeout, bounded queue, heap gate before TLS, normalise all
  detections into one schema (bbox x/y/width/height normalised 0..1).
- Failure raises `AiProviderFailed` and leaves the camera untouched.

### MQTT / Home Assistant
- Own worker task, `esp-mqtt`, MQTT 3.1.1; never block camera tasks.
- `visioncam/{device_id}/…` topics with a stable `device_id`; retained
  `online` + retained Last Will `offline` (≥ QoS 1).
- Retained discovery with stable `unique_id`s; handle the HA birth message;
  clean up removed entities with an empty retained config; never republish
  every loop.
- Never publish secrets or binary images.

### Frontend
- Vite build embedded in firmware; no runtime internet dependency.
- Typed API client only; HTTP-only cookie session; no credentials in
  browser storage; paginate media; keep the stream out of the query cache.
- Provide loading, empty, error, degraded, toast, auth and confirm states on
  every page; mobile layouts must work.
- Measure bundle size before adding fonts or icon sets.

### Reference-first (clean-room)
1. Look up the subsystem in the SPEC §1A.8 reference map.
2. Read the R1 implementation and R2 API; write in **your own words** what it
   does, its limits and its failure modes.
3. Decide adopt / adapt / defer / reject, checking the licence policy
   (SPEC §1A.2) first — R1 is AGPL-3.0.
4. Implement from the specification and your written notes, **not** from R1's
   text.
5. Compare behaviour and numbers with the R1 baseline on hardware.
6. Record decision, evidence and divergence in `docs/REFERENCE_ANALYSIS.md`.
7. If R1 and the specification disagree, the specification wins; flag it to
   the owner.

## Troubleshooting sequence (crash or instability)

1. Check reset reason and serial logs (RTC RAM ring survives soft resets).
2. Check free heap, minimum heap, largest free block and PSRAM.
3. Check task stack high-water marks.
4. Disable AI.
5. Disable external network storage.
6. Disable continuous recording.
7. Test camera alone.
8. Test SD alone.
9. Test camera + SD.
10. Re-enable one subsystem at a time.

Never "fix" instability by increasing arbitrary delays without identifying
the resource or synchronisation issue. If it is a crash loop, suspect the
power supply first — that is R1's own finding.

## Definition of done

Compiles · tests pass · hardware behaviour validated or marked `UNVERIFIED` ·
resource impact measured · error and degraded paths present · docs and
`CHANGELOG.md` updated · no secrets or unsafe defaults.
