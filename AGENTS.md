# AGENTS.md — VisionCam Pro

Instructions for any coding agent working in this repository. Loaded
automatically by OpenCode. Read `SKILL.md` (see below) and the relevant
`docs/` before changing anything.

---

## Project mission

Build **VisionCam Pro**, a standalone, local-first surveillance camera for the
AI-Thinker ESP32-CAM (OV2640, microSD, ESP32-CAM-MB) using PlatformIO with
`framework = espidf` (native ESP-IDF, **no Arduino layer**), and an embedded
React + TypeScript web UI.

The key promise: *the camera keeps capturing, recording and managing local
media when internet, MQTT, external AI or network storage are unavailable.*

## Where to start

| Need | Read |
|---|---|
| What phase am I on, what is next | `.opencode/plans/README.md` |
| The active work order | `.opencode/plans/0N-phase-*.md` |
| Why the project is shaped this way | `.opencode/plans/00-analysis.md` |
| Requirements for a subsystem | `SPEC.md` (section named in the phase file) |
| UI rules and tokens | `DESIGN.md` (authoritative for UI) + `mockup-ui/*.png` |
| Test/acceptance status | `.opencode/plans/11-acceptance-matrix.md`, `docs/TEST_PLAN.md` |
| Reuse decisions for a subsystem | `docs/REFERENCE_ANALYSIS.md` |

Load a skill with the `skill` tool: `visioncam-dev` (main),
`visioncam-build-flash`, `visioncam-reference-first`, `visioncam-webui`,
`visioncam-api-contract`, `visioncam-hardware-validation`.

## Authority order when documents disagree

1. `SPEC.md` — except where it explicitly supersedes itself (see
   `.opencode/plans/00-analysis.md` §4).
2. `DESIGN.md` — UI only; overrides the mockup and SPEC §5/§16.
3. `mockup-ui/*.png` — layout/visuals; `[bracketed]` values are placeholders,
   not data.
4. `reference/` — behavioural evidence only, never code.

## Mandatory rules

1. Read `SKILL.md` and the relevant `docs/` before making changes.
2. Never assume the target is ESP32-S3. It is the original ESP32.
3. Never assume arbitrary TFLite models fit or run.
4. Never promise native H.264/MP4 recording on the ESP32-CAM.
5. Keep all core functionality operational without MQTT, external AI,
   internet, or network storage.
6. Use the AI-Thinker pin mapping from the board profile only.
7. Use the ESP-IDF SDMMC host in 1-bit mode as the initial supported SD
   configuration.
8. Treat GPIO4/flash LED and SD interaction as a hardware constraint.
9. Only the frame manager may acquire/release camera driver frame buffers.
10. Only the storage manager may perform filesystem operations.
11. Never block camera capture/stream tasks on MQTT, external AI, or network
    storage.
12. Every queue must have a bounded capacity and documented overflow
    behaviour.
13. Never allocate unbounded memory based on user input or media size.
14. Do not load the entire media index into RAM.
15. All media APIs must use opaque media IDs, not arbitrary filesystem paths.
16. All management, media, snapshot, and stream endpoints require
    authentication.
17. Never store or log plaintext passwords, tokens, or integration secrets.
18. Do not store session tokens in localStorage or URL parameters.
19. Validate every configuration update before persistence.
20. Preserve last-known-good model and configuration during updates.
21. Use versioned schemas for configuration, media index, event index, and
    API payloads.
22. Every feature must have a test or a documented hardware validation
    procedure.
23. Do not introduce a library without checking ESP32 compatibility,
    flash/RAM cost, maintenance, and license.
24. Pin dependencies; do not use floating `latest` versions.
25. Keep firmware modules small and independently testable.
26. Never claim a test passed unless it was actually run.
27. Do not silently remove existing features or change API contracts.
28. Update docs and changelog when behavior or contracts change.
29. Do not expose the device directly to the public internet.
30. Prefer safe degradation over keeping every feature active under memory
    pressure.
31. Before designing a subsystem, read SPEC Section 1A and the mapped R1/R2
    code; record adopt/adapt/defer/reject in `docs/REFERENCE_ANALYSIS.md`.
32. Do not copy or mechanically translate R1 (AGPL-3.0) code unless the owner
    has chosen the AGPL option in writing. Never vendor R1 into the build.
33. Treat `espressif/esp32-camera` as a pinned dependency; never fork it or
    link two copies.
34. Treat R1's performance numbers as unverified targets until the Phase 0
    baseline on the owner's board reproduces them.
35. Do not import R1 behaviours that conflict with this specification (Basic
    auth, UI on SD, images over MQTT, playback refusal during recording,
    header-only-at-close recording) without a recorded owner decision.
36. Do not claim a feature is "like R1" unless it was compared against the
    pinned R1 commit.
37. Build with PlatformIO `framework = espidf` only. Do not add the Arduino
    core as a component without a recorded owner decision. Port R1's Arduino
    API usage using SPEC §1A.10.

## Required workflow

**Before implementation (reference step):**
- Read SPEC Section 1A.
- Flash and measure the R1 baseline on the target board.
- Confirm which `esp32-camera` copy, PlatformIO platform version and
  ESP-IDF version will be used.

**Before implementation:**
- Inspect repository and current platform versions.
- Confirm board profile and camera pin mapping.
- Confirm partition layout and flash capacity.
- Verify PSRAM detection.
- Verify SD 1-bit mounting.
- Create a minimal build and hardware smoke test.

**For each task:**
1. State the intended module and interface changes.
2. Identify memory, timing, and hardware implications.
3. Implement the smallest vertical slice.
4. Compile firmware and web UI.
5. Run available unit/static tests.
6. Test on hardware when hardware behavior is involved.
7. Update docs and tests.
8. Report completed work and remaining limitations honestly.

## Coding standards

**Firmware** (`firmware/`):
- C++ with clear ownership and RAII where practical.
- Avoid exceptions and dynamic allocation in hot paths.
- Use fixed-size or bounded containers for real-time queues.
- Check all return codes; use explicit timeouts.
- Keep ISR code minimal; use the task watchdog correctly; avoid long critical
  sections.
- Never log secrets or high-frequency frame data.
- Use compile-time board configuration; pin constants live only in
  `board_profile.h`.

**Frontend** (`webui/`):
- React + TypeScript, functional components, typed API client.
- Centralized error handling; accessible controls; responsive layout.
- No external runtime CDN dependencies; no credentials in browser storage.
- Keep the bundle size appropriate for embedded flash; lazy-load large pages.

## Definition of done

A change is done only when:
- Code compiles (firmware **and** web UI).
- Relevant tests pass.
- Hardware-sensitive behavior is validated or explicitly marked
  `UNVERIFIED`.
- Resource impact is measured or estimated.
- Error and degraded paths are implemented.
- Documentation and `CHANGELOG.md` are updated.
- No secrets or unsafe defaults are introduced.

## Working agreement with the plan

- One phase at a time; never start phase N+1 with a red gate in phase N.
- Record adopt/adapt/defer/reject decisions, ADRs and measured numbers as
  you go — do not batch them at the end.
- If SPEC and reality disagree on hardware, the hardware wins: record the
  difference and flag it to the owner instead of papering over it.
