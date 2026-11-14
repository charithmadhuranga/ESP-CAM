# Phase 3 — Recording and playback

**Goal:** crash-tolerant MJPEG-AVI segment recording (continuous, manual and
event-ready) plus server-side timed replay that never stalls a recording.

**Spec:** §4.6 (AVI writer design, playback decision, policies), §6.4, §8.1,
§11.5, §11.10, §12 Phase 3.
**Skills:** `visioncam-dev`, `visioncam-api-contract`,
`visioncam-hardware-validation`.

---

## Entry criteria

Phase 2 exit gate green; storage writer path measured.

**Read first:** SPEC §4.6 in full and the mapped R1 code — `avi.cpp`,
`mjpeg2sd.cpp` (`saveFrame`, `closeAvi`, `openSDfile`, `getNextFrame`) —
via `visioncam-reference-first`. Derive the AVI structure from the public
RIFF/AVI format, **never** from R1's byte table (licence rule).

## Deliverables

1. `firmware/src/recording/` AVI/MJPEG segment writer.
2. Dedicated storage-writer task with bounded frame queue + backpressure.
3. Continuous recording, manual recording, event-recording foundation.
4. Segment rotation at duration/size/frame-count thresholds.
5. Recording index + opaque recording IDs.
6. Crash-recovery scanner for interrupted segments.
7. Server-side timed MJPEG replay + seek-by-frame-offset.
8. Recording browser UI with player, timeline, integrity badge, download,
   delete.

## Ordered tasks

### 3.1 Writer

1. RIFF/AVI container built from the public format: reserved fixed-size
   header region at offset 0, `movi` list of `00dc` JPEG chunks each padded
   to a 4-byte boundary, trailing `idx1` index of 16 bytes per frame.
2. Index built in a **PSRAM** buffer sized `(maxFrames + 2) * 16`; bound the
   per-segment frame count so it cannot overflow — **rotate the segment
   instead of rebooting**.
3. Stage frames in an 8 KB (sector-multiple) RAM buffer; write whole buffers
   only. Never many small writes.
4. Temporary in-progress name → rename to the final self-describing name
   (date/time, frame size, **measured** FPS, duration) only after header and
   index are written. Per-day folders; suffixes `_C` continuous, `_T`
   timelapse, event marks in the index.
5. Header stores **measured** FPS (frames / elapsed), not requested FPS.
6. Delete segments shorter than the minimum duration (default 5 s) unless
   event-marked.
7. Writer task is the only file writer; mutex only around header patching,
   never around frame writes.
8. Backpressure: bounded queue; if the card cannot keep up, drop frames and
   count them, or stop recording safely — never exhaust RAM.

### 3.2 Crash recovery (VisionCam improvement over R1)

9. Default segment 60 s (configurable) so loss is bounded.
10. Write a provisional header at segment start.
11. On boot, scan `*.tmp`, walk `00dc` chunks sequentially (layout is
    self-describing), stop at the first truncated chunk, truncate there,
    rebuild `idx1` + header, rename to the final name with a `recovered`
    flag in the index.
12. **Never** delete an unrecovered segment automatically.

### 3.3 Recording modes

13. Continuous recording **disabled by default**; configurable enable, FPS,
    frame size, segment duration, schedule, retention, max storage percent
    reserved for continuous footage.
14. Manual start/stop.
15. Event-recording foundation: hooks the event bus will drive in Phase 4
    (create event → snapshot → segment/mark → post-event duration →
    cooldown). Pre-event buffer default **0** until RAM is measured (G15).

### 3.4 Playback

16. Server-side replay: read the AVI in sector-multiple blocks, walk `00dc`,
    stream `multipart/x-mixed-replace` at the **recorded** FPS. Seek by
    restarting at a frame offset from `idx1` (`start_frame` / `start_ms`).
17. Concurrency rule (deliberate divergence from R1): playback must **never**
    stop or stall recording. Playback runs at lower priority than the writer
    and is allowed only within a configured share of measured card
    throughput; otherwise return `503 PLAYBACK_BUSY`. Playback consumes one
    of the limited stream slots.

### 3.5 API and UI

18. Endpoints §6.4: status, config get/put, start, stop, list (paginated),
    metadata, download, `play`, delete.
19. **Recordings** page: 240px sidebar with calendar + filters
    (continuous/motion/AI/manual), player, timeline with event markers,
    Play/Pause/Speed/Download/Delete, integrity badge
    (`ready` / `recovered` / `failed`).
20. Settings › Recording form per `SettingsRecording.png`.

## Memory / timing / hardware notes

- Expect ≈20 fps VGA/SVGA and ≈5 fps XGA+ on a good Class 2+ card (R1
  targets, unverified until Phase 0 baseline exists). Default event FPS well
  below those limits until measured.
- Index buffer in PSRAM; segment frame cap derived from segment seconds ×
  expected FPS with headroom.
- Live view must stay responsive during recording — measure stream FPS with
  recording on and off.

## Tests and acceptance

- [ ] Continuous recording starts/stops; manual recording starts/stops.
- [ ] Segment closes at the configured duration and at the frame cap.
- [ ] AVI validates in desktop media tools (`ffprobe`/VLC) — run it, do not
      assume.
- [ ] Header FPS equals measured FPS within 5 % (SPEC §11.10).
- [ ] Frame timestamps and rate are reasonable; downloaded segment plays in
      a compatible player.
- [ ] Segment interrupted by power loss is recovered by the chunk scanner
      and plays back.
- [ ] Playback works through the custom player, including seek.
- [ ] Playback never stops or visibly stalls an active recording, or returns
      `PLAYBACK_BUSY`.
- [ ] Live view remains responsive during recording (measured FPS).
- [ ] R1 parity: recording FPS at VGA and QVGA within the agreed tolerance of
      the Phase 0 baseline on the same card.

## Exit gate

Valid AVI segments, safe recovery after interrupted shutdown, responsive live
view during recording, server-side playback working.

## Docs updated

`docs/STORAGE_AND_RECORDING.md` (format, naming, recovery procedure),
`docs/API.md`, `docs/UI_UX_SPEC.md`, `CHANGELOG.md`.

## Risks / known gaps

- Header/index-only-at-close is R1's weakness; the provisional header +
  scanner must be proven with a real power cut, not a software kill.
- Seek accuracy depends on `idx1` correctness — validate with an externally
  generated reference AVI.
