# Phase 9 — Hardening and release

**Goal:** prove the system survives 24 hours of mixed workload and repeated
power interruption, is secure within its documented limits, migrates
configuration safely, and ships with complete release documentation.

**Spec:** §9, §10, §11 (all), §17, §12 Phase 9.
**Skills:** `visioncam-hardware-validation`, `visioncam-dev`.

---

## Entry criteria

Phases 0–8 gates green. Owner has provided the deployment SD card model
(**D5**).

## Deliverables

1. Resource optimisation with measured before/after numbers.
2. 24-hour mixed-workload test report.
3. Security review against SPEC §10 + documented threat model.
4. Power-loss recovery verification (repeated, not once).
5. Configuration migration (versioned schema + rollback).
6. Release documentation set (§17) and a versioned firmware release.

## Ordered tasks

### 9.1 Resource optimisation

1. Measure and record: firmware size, web asset size, free flash/partition
   headroom, minimum free heap during representative workload, PSRAM use,
   stream FPS, recording FPS, AI inference duration per model, SD write
   performance, task stack high-water marks.
2. Reduce load in the documented order when memory pressure appears: lower
   inference rate → lower stream FPS → disable timelapse generation → stop
   optional uploads → stop recording safely. Never corrupt a file to keep a
   feature alive.
3. Confirm crash-loop detection and the RTC RAM log survive soft resets and
   surface the reset cause.

### 9.2 Degraded-mode matrix (SPEC §9) — exercise every row

Wi-Fi unavailable · MQTT unavailable · external AI unavailable · SD missing ·
SD full · SD write error · invalid TFLM model · AI over budget · time not
synchronised · browser disconnected · power loss · network storage
unavailable · authentication failure. In each case the core camera must keep
working and the UI must name the failed subsystem in a degraded banner.

### 9.3 Security review (SPEC §10, all 23 items)

4. Verify: forced default-password replacement, salted hashes, CSPRNG session
   tokens, HTTP-only cookies, all media/API/stream endpoints protected,
   login rate-limiting, JSON + multipart validation, upload size caps, no
   path traversal, no raw filesystem paths exposed, secrets masked and
   unlogged, TLS validation for external HTTPS, no public unauthenticated
   stream URLs, CSRF on cookie-authenticated state changes, Origin/Host
   validation, confirmation for format/bulk delete/credential reset, no
   arbitrary firmware/model execution, LAN-only default, **no on-device TLS**
   in v1, no R1 Basic-auth model, no port-forwarding/heartbeat shortcuts.
5. Write the threat model and limitations: this is not a hardened commercial
   NVR.

### 9.4 Power-loss and migration

6. Repeated power-interruption tests during recording, snapshot write and
   index append; confirm recovery and that no unrecovered segment is ever
   auto-deleted.
7. Configuration migration: versioned schema, migration functions,
   last-known-good preserved across updates, rollback if apply fails.

### 9.5 Long-run test

8. ≥24 h mixed workload: live streaming + event recording + snapshots +
   motion + MQTT reconnects + storage usage + timelapse.
9. Track free heap, minimum heap, largest free block, stack watermarks,
   dropped frames, SD errors, reset count. Acceptance: no unexplained resets,
   no unbounded memory growth.

### 9.6 Release

10. `docs/` set: PRODUCT_SPEC, ARCHITECTURE, HARDWARE, API,
    MQTT_HOME_ASSISTANT, STORAGE_AND_RECORDING, AI_MODELS, SECURITY,
    TEST_PLAN, UI_UX_SPEC, REFERENCE_ANALYSIS + install, first-login,
    Wi-Fi setup, SD formatting, backup/restore, troubleshooting,
    known-issues guides.
11. Release package: firmware binary + version/build metadata, web UI
    version, board/SD notes, flashing instructions, plus the §17 measurement
    report.
12. Tag a versioned release; update `CHANGELOG.md`.

## Tests and acceptance

- [ ] 24-hour mixed workload completed with the tracked metrics recorded.
- [ ] No unexplained resets; no unbounded heap growth.
- [ ] Media recovery verified across repeated power interruptions.
- [ ] Every SPEC §9 degraded row behaves as specified.
- [ ] Every SPEC §10 security item verified or explicitly documented as a
      limitation.
- [ ] All optional integrations fail gracefully.
- [ ] Configuration migration + rollback tested from an older schema.
- [ ] R1 parity tests (SPEC §11.10) all pass or are explained.
- [ ] Release measurement report complete (§17).

## Exit gate

24-hour test green, security review closed, power-loss recovery verified,
release docs complete, versioned firmware published.

## Docs updated

All of `docs/`, `CHANGELOG.md`, release notes.

## Risks / known gaps

- Long-run failures are usually power supply (R1's own finding) — check the
  5 V rail before blaming the firmware.
- Do not report any performance number without the matching Phase 0 baseline
  row (SPEC §1A.7 rule 4).
