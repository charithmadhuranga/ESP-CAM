# Phase 5 — Timelapse

**Goal:** scheduled JPEG capture into per-project AVI (direct-to-AVI by
default), with progress, preview, download and optional later re-generation.

**Spec:** §4.7, §6.6, §12 Phase 5 (+ amendment).
**Skills:** `visioncam-dev`, `visioncam-api-contract`.

---

## Entry criteria

Phase 4 exit gate green; time synchronisation available (§4.12).

**Read first:** SPEC §4.7 and R1 `mjpeg2sd.cpp` (`timeLapse`) via
`visioncam-reference-first`.

## Deliverables

1. `firmware/src/timelapse/` scheduler + project manager.
2. Direct-to-AVI mode (default) sharing the Phase 3 header/index code with
   its own index buffer.
3. Source-frame index; optional frames+generate mode.
4. Progress, cancellation, preview, download, delete.
5. Timelapse UI page.

## Ordered tasks

### 5.1 Project model

1. `project.json` per project under `/timelapse/{project_id}/` with
   `schema_version`: name, state (`capturing`/`ready`/`stopped`), interval,
   start/end schedule, resolution, quality, playback FPS, retention,
   counters, timestamps.
2. Projects survive reboot: on boot, resume `capturing` projects only if the
   schedule is still valid and time is synchronised.

### 5.2 Capture

3. Do **not** start a timelapse until time is synchronised; otherwise mark
   `unsynchronized` (§4.12) and refuse with a clear reason.
4. Direct-to-AVI: append each captured frame to the timelapse AVI; segments
   close at the configured duration or frame cap; playback FPS stored in the
   header. No generation job.
5. Use the low-rate camera mode (`fb_count = 1`, on-demand) when no stream or
   recording is active and the interval is long — **verify mode switching is
   reliable before shipping**; otherwise stay in the shared capture loop and
   record why.
6. Coexistence with continuous recording: allow only if Phase 5 measurements
   show enough SD/CPU headroom; otherwise enforce mutual exclusion with a
   clear UI message (never silently).

### 5.3 Optional frames + generate

7. Keep individual JPEG source frames under `frames/` and index them, for
   users who want to re-render at a different FPS.
8. Background generation job with progress, cancellation and storage checks.
   Never generate at high resolution/long duration while streaming or
   continuous recording is under heavy load.

### 5.4 API and UI

9. Endpoints §6.6: projects list/create/get/update, start, stop, generate,
   job progress, preview, download, delete.
10. **Timelapse** page per `Timelapse.png`: project list with
    capturing/ready status + progress bar, 340px new-project form
    (interval, schedule, resolution/quality, output FPS), preview, download,
    manage source frames, cancel.

## Memory / timing / hardware notes

- Timelapse shares the capture loop and the storage writer — contention is
  the main risk. Measure stream FPS and recording FPS with a timelapse
  running.
- Direct-to-AVI avoids a heavy generation step; keep it the default.

## Tests and acceptance

- [ ] A short project captures and produces output without disrupting the
      camera (stream and recording FPS measured before/during).
- [ ] Scheduled capture occurs at the configured interval.
- [ ] Source frames persist; project metadata survives reboot.
- [ ] Generation progress visible; cancellation works.
- [ ] Output frame order correct; output downloadable and playable.
- [ ] Generation does not corrupt concurrent recording.
- [ ] Refuses to start with unsynchronised time, with a clear message.
- [ ] Mutual exclusion (if enforced) is explained in the UI.

## Exit gate

Short timelapse captured and produced with no disruption to camera or
recording.

## Docs updated

`docs/STORAGE_AND_RECORDING.md`, `docs/API.md`, `docs/UI_UX_SPEC.md`,
`CHANGELOG.md`.

## Risks / known gaps

- Switching the camera between `fb_count=1` and `fb_count=2` at runtime may
  be unreliable — treat as an experiment with a fallback.
- Long-duration projects need a retention story (ties into Phase 9).
