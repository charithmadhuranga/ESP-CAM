# Phase 4 — Motion and event engine

**Goal:** a lightweight, tunable motion detector feeding a bounded typed event
bus that drives snapshots, event recording, UI indicators and (later) MQTT.

**Spec:** §4.3, §4.3.1, §8.3, §6.8, §11 (motion behaviour), §12 Phase 4.
**Skills:** `visioncam-dev`, `visioncam-api-contract`.

---

## Entry criteria

Phase 3 exit gate green (event recording hooks exist).

**Read first:** SPEC §4.3.1 and R1 `motionDetect.cpp` via
`visioncam-reference-first` — then write it new.

## Deliverables

1. `firmware/src/motion/` motion engine (independent of AI).
2. Threshold / sensitivity / cooldown / sample-rate configuration.
3. `firmware/src/core/` typed, bounded event bus.
4. Motion snapshots and event recording triggers.
5. Motion debug overlay for calibration.
6. Ambient light level with hysteresis (from the same decode).
7. Event list + UI indicators.

## Ordered tasks

### 4.1 Motion engine

1. Decode JPEG to a **very small grayscale bitmap** using R2/IDF JPEG helpers
   — no full-resolution RGB buffers; the downscale is the noise filter.
2. Compare per-pixel intensity against the previous sample with a
   configurable threshold; compute the changed-pixel ratio; require
   `minimum_changed_ratio`; apply temporal smoothing, a settle counter and a
   cooldown.
3. **Two sample rates** (R1 pattern), both configurable: higher while idle
   (default ≈5 checks/s), lower while recording (default 1 check/2 s, which
   also defines the post-motion hold time).
4. Ignore the warm-up frames while exposure settles.
5. Not offered above **SXGA** (decoder limit) — the UI must disable the
   option and say why.
6. Derive ambient light level from the same decode; hysteresis + settle
   counter for night-mode decisions.
7. Trigger-source bitmask (camera, manual, future PIR) recorded on every
   event so the reason is known.
8. **Debug view** overlaying changed pixels in red for threshold
   calibration.
9. Motion detection runs **while continuous recording runs** (deliberate
   divergence from R1) so events still get marked.
10. Benchmark CPU and RAM alongside streaming; record the numbers.

### 4.2 Event bus

11. Typed events: `MotionStarted`, `MotionEnded`, `ObjectDetected`,
    `SnapshotCreated`, `RecordingStarted`, `RecordingStopped`,
    `StorageWarning`, `StorageError`, `MqttConnected`, `MqttDisconnected`,
    `AiProviderFailed`, `NetworkStorageUploadFailed`, `CameraFailed`.
12. **Bounded** queue with documented overflow behaviour (AGENTS rule 12);
    payloads small; image data referenced by media ID only, never queued.
13. Subscribers: snapshot manager, recorder, MQTT worker (Phase 7), UI push,
    index writer.

### 4.3 Event recording policy

14. On event: create event record → save event snapshot → start/mark segment
    → record for `post_event_seconds` → apply cooldown → publish → update UI
    event list.
15. Minimum-duration rule: event recordings shorter than the minimum are
    deleted unless marked.
16. Event JSONL index records with `detections[]` and
    `notification_status`.

### 4.4 API and UI

17. Endpoints §6.8: motion config get/put, motion status, events list
    (paginated).
18. Overview: "Motion today" stat card, recent events list with Motion/AI
    tags.
19. Settings › Motion per `SettingsMotion.png`: sensitivity, threshold,
    detection-zone editor (mask is a future iteration — provide the config
    shape and a read-only placeholder), cooldown, minimum frames,
    start-recording and MQTT-notify toggles.
20. Live View status strip: `Motion Detection ON/OFF`.

## Memory / timing / hardware notes

- The low-res grayscale buffer is small and fixed; never allocate it from
  user input.
- Motion decode must not compete with the storage writer — measure decode
  time at each frame size (R1 reference: 80 ms VGA, 120 ms SVGA).
- Label clearly in the UI: **motion detection is not object recognition**.

## Tests and acceptance

- [ ] Configurable motion event triggers a snapshot and a recording.
- [ ] False-trigger rate can be tuned; no triggering on sensor noise.
- [ ] No unbounded event queue — overflow behaviour verified by flooding.
- [ ] Cooldown respected; no duplicate snapshots inside cooldown.
- [ ] Motion option disabled above SXGA with an explanatory message.
- [ ] Motion works while continuous recording is active.
- [ ] Debug overlay renders and helps calibrate a threshold.
- [ ] Warm-up frames ignored.
- [ ] Event list paginates and survives reboot.

## Exit gate

Configurable motion triggers snapshot + recording with a bounded queue and a
tunable false-trigger rate.

## Docs updated

`docs/ARCHITECTURE.md` (event bus contract), `docs/STORAGE_AND_RECORDING.md`,
`docs/API.md`, `docs/UI_UX_SPEC.md`, `CHANGELOG.md`.

## Risks / known gaps

- Zone editor is specified as a future iteration — do not over-build it.
- JPEG decode cost scales with frame size; validate the idle/recording sample
  rates at the largest supported motion frame size.
