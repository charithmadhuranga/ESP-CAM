# Phase 6 — AI framework

**Goal:** a provider abstraction where **external HTTP AI is the primary
supported path**, TFLite Micro is experimental and may legitimately be
declared unsupported, and no AI failure can stop the camera.

**Spec:** §4.4, §4.4.1, §4.4.2, §4.4.3, §6.8, §9, §12 Phase 6 (+ amendment).
**Skills:** `visioncam-dev`, `visioncam-api-contract`.

---

## Entry criteria

Phase 4 exit gate green (event bus + motion exist).

**Read first:** SPEC §4.4 and R1's ML usage pattern via
`visioncam-reference-first`. Adopt R1's *pattern* (classify after motion to
decide whether to keep an event), not per-frame classification.

## Deliverables

1. `firmware/src/ai/` with `IInferenceProvider` and three providers:
   `NoAiProvider` (default), `ExternalAiProvider` (primary),
   `TfliteMicroProvider` (experimental).
2. Model manifest schema + validator, staged upload, activation/rollback.
3. Normalised detection event schema.
4. External AI client with timeout, bounded queue, retry policy, TLS
   verification setting.
5. AI dashboard UI + Settings › AI Models.

## Ordered tasks

### 6.1 Provider interface

6. Implement the §4.4 interface exactly:
   `begin() / isReady() / infer(FrameView&) / stop() / providerId()`.
7. `NoAiProvider` is the default and must always be selectable so AI can be
   fully disabled.
8. AI task priority **lower** than streaming and storage safety; inference at
   a configurable low rate; never block streaming/storage on inference.

### 6.2 Model lifecycle (TFLM)

9. Manifest schema per §4.4.1 (`schema_version`, `model_id`, `format`,
   `file`, `sha256`, input/output tensors, `required_tensor_arena_bytes`,
   `operators`, `classes`, `preprocessing`).
10. Validator: header, length, checksum, supported operators, input/output
    shape, quantisation, tensor-arena requirement, arena allocation
    failure handling.
11. Upload streamed to `/models/staging/` with a hard size cap — **never
    buffer a whole model in RAM**.
12. Activation never overwrites the active model; keep
    `/models/backup/` as last-known-good; provide rollback and deactivate.
13. Compatibility status + validation error surfaced in the UI.

### 6.3 External AI

14. Config: base URL, auth token, timeout, upload resolution, JPEG quality,
    inference interval, selected event types, retry policy, TLS verification
    (on for production), max upload size.
15. `POST /api/v1/infer`, multipart JPEG + metadata; parse the §4.4.2
    response shape; normalise detections into the §4.4.3 schema (bbox
    x/y/width/height, normalised 0..1, consistent everywhere).
16. Policies: `disabled`, `motion_only`, `periodic`, `on_device_event`,
    `manual_snapshot`.
17. Offline behaviour: queue only a small **bounded** number of requests or
    drop per policy; never accumulate images in RAM.
18. Heap gate before any TLS session (≈64 KB free + largest-block check);
    back off otherwise.
19. Failure raises `AiProviderFailed`, disables remote inference temporarily,
    and leaves motion/local operation intact.

### 6.4 API and UI

20. Endpoints §6.8: ai status, providers, models list/upload/validate/
    activate/deactivate/rollback, ai events, ai config get/put.
    - **Added by D6 (2026-11-20):** `GET/PUT /ai/config` carry
      `confidence_threshold` (fixed-point in JSON) and `auth_token_set`
      — the token itself is never echoed (AGENTS rule 17). Provider
      switching through `PUT {"provider": ...}` applies live, no reboot.
21. **AI Detection** page per `AI.png`: Provider card (TFLite Micro /
    External / Disabled pills, active model, compatibility, Upload model,
    Rollback), Detection card (confidence threshold, detection interval,
    classes), External AI server card (endpoint, status, Test connection),
    Recent detections, Metrics (inference time, tensor arena).
    - **D6:** the pills are the *only* way a user selects a provider —
      `none` (Disabled), `external_ai`, `tflite_micro`. Selecting
      `tflite_micro` shows the D4 *Unavailable* state, not an error.
22. Settings › AI Models per `SettingsAIModels.png`.
23. Overview "AI events today" card; Live View `AI Detection ON/OFF` pill.

### 6.5 D6 wiring (owner directive, 2026-11-20)

24. `aiManagerInit()` runs on **every boot** (no storage gate); a fresh
    device persists `enabled=true, provider=none` to NVS on first boot.
25. Motion-triggered inference: the AI task subscribes to the event bus
    (`MotionStarted`/`MotionEnded` → task notify only, per
    `event_bus.h` contract), acquires a frame lease, **copies** the JPEG
    into a bounded 256 KB buffer and releases the lease before any
    provider I/O (SPEC §4.1.1 rule 9). Rate-limited by
    `inference_interval_ms`; `provider=none` idles without taking frames.
26. Confidence filter applied in `aiManagerInferFrame` — sub-threshold
    detections are never recorded or published (SPEC §7 `ai.confidence_threshold`).
27. Model upload streams to `*.tflite.part` via `storageAppendFile` and
    renames into place only when complete; IDs validated against
    `[A-Za-z0-9_-]{1,48}` before becoming a path segment.
28. Frame registry sized for the new consumer (`VC_FRAME_SUBSCRIBERS` 4→6:
    smoke, recorder, motion, timelapse, ai persistent + snapshot
    transient).

## Memory / timing / hardware notes

- Tensor arena must be **fixed or bounded** and accounted for against free
  heap before activation.
- Benchmark inference duration, peak memory and frame impact; record per
  model in `docs/AI_MODELS.md`.
- R1 states ML on the original ESP32 is only feasible on S3 — treat any
  TFLM success as an exception that must be re-measured, not a promise.

## Tests and acceptance

- [ ] No-AI mode works and AI can be fully disabled.
- [ ] Invalid model safely rejected (bad checksum, unsupported operator,
      arena too large) with a readable validation error.
- [ ] Last-known-good model remains recoverable after a failed activation.
- [ ] Inference rate configurable; AI load does not crash streaming
      (measured).
- [ ] External AI timeout/retry works; outage does not stop local recording.
- [ ] Detection events normalise to one schema from every provider.
- [x] On-device AI declared unsupported for this build — **D4 answered
      2026-11-20**: selectable in the UI, runtime not linked, honest
      `provider_ready=false` + HTTP 400 on activation.
- [ ] **D6:** fresh boot shows `enabled=true, provider=none` persisted;
      switching `none → external → tflite_micro` over HTTP works without
      reboot; a >1 KB model upload round-trips byte-exact; motion triggers
      an inference attempt while `provider=none` takes no frames.

## Exit gate

Invalid model rejected; benchmarked model runs or TFLM declared unsupported
(**D4: answered — unsupported, selectable**); external AI failure leaves
the core camera untouched.

## Docs updated

`docs/AI_MODELS.md` (manifest, operators, arena, benchmark numbers),
`docs/API.md`, `docs/UI_UX_SPEC.md`, `docs/REFERENCE_ANALYSIS.md` (TFLM
decision), `CHANGELOG.md`.

## Risks / known gaps

- TFLM on the original ESP32 is the highest-uncertainty item in the whole
  plan; budget it as an experiment with a clean "unsupported" exit.
- Model-specific output decoders are not generic — a manifest without a
  matching decoder must fail validation.
