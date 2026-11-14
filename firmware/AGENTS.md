# AGENTS.md — `firmware/`

Loaded when working under `firmware/`. Read the repository-root `AGENTS.md`
first; this file only adds firmware-specific guidance.

## Layout

```
firmware/
├── include/            # board_profile.h and shared headers
├── src/
│   ├── app/            # init order, main loop
│   ├── core/           # event bus, resource monitor, RTC RAM log, config
│   ├── camera/         # camera manager (init, settings, recovery)
│   ├── frame/          # frame manager — SOLE owner of camera buffers
│   ├── network/        # Wi-Fi, NTP, reconnect
│   ├── web/            # esp_http_server, routes, WS, static assets
│   ├── auth/           # passwords, sessions, CSRF, throttling
│   ├── motion/         # motion engine
│   ├── ai/             # IInferenceProvider + providers
│   ├── recording/      # AVI writer, playback
│   ├── snapshot/       # snapshot manager
│   ├── timelapse/      # scheduler + projects
│   ├── storage/        # storage manager — SOLE owner of the filesystem
│   ├── mqtt/           # esp-mqtt worker
│   ├── integrations/   # network storage backends
│   └── system/         # metrics, restart, migration
└── test/
```

## Ownership rules (enforce in review)

- `frame/` is the only module that calls `esp_camera_fb_get` /
  `esp_camera_fb_return`.
- `storage/` is the only module that opens, writes, renames or deletes files.
- `camera/` owns `board_profile.h` pin usage; no other file may define a pin.
- `web/` never talks to the filesystem or the camera directly — it calls
  manager interfaces.
- Nothing outside `core/` defines global mutable state.

## Conventions

- ESP-IDF style: `esp_err_t` return codes checked at every call site; `ESP_LOGx`
  with tags per module; explicit `pdMS_TO_TICKS` timeouts.
- No exceptions, no RTTI assumptions, no unbounded `new` in hot paths.
- Every FreeRTOS queue: fixed depth, named, with a documented overflow
  policy (drop-oldest / block-with-timeout / reject) stated in a comment
  next to the create call.
- Every task: priority and stack size named constants in one header, with the
  measured stack high-water mark recorded in `docs/ARCHITECTURE.md`.
- Long-running HTTP responses (stream, playback, large download) must release
  their stream slot on **every** exit path — use RAII.
- Secrets live in NVS under a separate namespace and are never returned by
  GET handlers (return `*_configured: true` booleans instead).
- Configuration updates: parse → validate → build candidate → persist →
  apply → roll back on failure → return sanitised effective config.

## Before you touch a subsystem

1. Read the phase file in `.opencode/plans/` that owns it.
2. Read SPEC §1A for that subsystem and the mapped R1/R2 code.
3. Write the adopt/adapt/defer/reject entry in `docs/REFERENCE_ANALYSIS.md`.
4. Implement from the spec and your own notes — never from R1's text.
