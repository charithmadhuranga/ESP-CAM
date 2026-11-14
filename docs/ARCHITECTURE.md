# Architecture — VisionCam Pro

Initial skeleton (Phase 0). Extended every phase; the resource table in
§4 is filled at the **end** of every phase (plan `README.md`, global rules).

## 1. Shape of the system

```
                    ┌─────────────────────────────────────────┐
                    │              firmware/                   │
   browser ──HTTP──►  esp_http_server + WS (auth, CSRF, limits)│
   (embedded UI)     └───────┬─────────────────────────────────┘
                             │ lease frames (never owns buffers)
                    ┌────────▼────────┐
                    │  frame manager  │  ← ONLY owner of camera fb acquire/return
                    └───┬─────────┬───┘
                        │         │
              ┌─────────▼──┐   ┌──▼──────────────┐
              │ camera mgr │   │ storage manager │ ← ONLY owner of filesystem I/O
              │  (R2 drv)  │   │  SD 1-bit, FATFS│
              └────────────┘   └──┬──────────────┘
                                 │
              ┌──────────────────┼───────────────────┐
              │ recorder(AVI)    │ snapshots │ timelapse │ retention
              └──────────────────┴───────────────────┘

   Optional, all fail-soft: MQTT/HA · external AI · network storage
   Core camera keeps running if every one of them is down (SPEC §9)
```

## 2. Non-negotiable ownership rules

| Resource | Sole owner | Rule |
|---|---|---|
| Camera frame buffers | frame manager | AGENTS rule 9 — nobody else calls `esp_camera_fb_get/return` |
| Filesystem | storage manager | AGENTS rule 10 — no other module does file I/O |
| Wi-Fi / HTTP | network manager | camera tasks never block on network (rule 11) |

Every queue is **bounded with documented overflow behaviour** (rule 12); no
allocation is sized from unbounded user input or media size (rule 13); the
media index is never fully loaded into RAM (rule 14) — see the deliberate
R1 divergence in `docs/REFERENCE_ANALYSIS.md`.

## 3. Module map (grows per phase)

| Module | Phase | Notes |
|---|---|---|
| `board_profile.h` | 0 | the only place pin constants exist |
| smoke test `main.cpp` | 0 | reset reason, PSRAM, heap reporting |
| camera manager | 1 | R2 init, PID check, warm-up, PWDN retry |
| frame manager | 1 | single acquire/release point |
| Wi-Fi manager | 1 | station mode; no internet required for core function |
| HTTP server + embedded UI | 1 | auth sessions, no tokens in localStorage |
| storage manager | 2 | SD 1-bit mount, FATFS VFS, no auto-format |
| media index | 2 | versioned, paginated, opaque media IDs |
| AVI recorder | 3 | short segments + provisional header |
| playback | 3 | never stalls recording; `503 PLAYBACK_BUSY` |
| motion / event engine | 4 | bounded event queue |
| timelapse | 5 | must not disrupt live capture |
| AI framework | 6 | external-first; camera never blocked |
| MQTT / Home Assistant | 7 | fail-soft, stable `visioncam/{device_id}` IDs |
| network storage | 8 | upload after segment close; local files preserved |
| hardening | 9 | 24 h mixed workload, degraded-mode drills |

## 4. Resource measurements (fill at the end of every phase)

Record **heap, largest free block, stack high-water marks and frame rates**.
Never estimate — paste real numbers or write `UNVERIFIED`.

| Phase | Free heap B | Min heap B | Largest block B | Stack HWM (worst task) | FPS measured | Date / build |
|---|---|---|---|---|---|---|
| 0 | UNVERIFIED | UNVERIFIED | UNVERIFIED | UNVERIFIED | UNVERIFIED | no board attached |

Build size after Phase 0 (host-side, verified 2026-11-15):

| Artifact | Value |
|---|---|
| RAM used | 13,932 B / 327,680 B (4.3 %) |
| Flash used | 183,784 B (17.5 % of the 1 MB app slot) |
| `firmware.bin` | 184,208 B |

## 5. Degraded modes (SPEC §9)

Prefer safe degradation over keeping everything alive under memory pressure:

| Failure | Behaviour |
|---|---|
| No SD card | camera + live view + diagnostics keep working; storage features disabled with a clear message |
| No Wi-Fi / no internet | local capture, recording, playback all continue |
| MQTT broker down | recording unaffected; events queue boundedly then drop with a log |
| External AI unreachable | frames not blocked; on-device path or skip per config |
| Network storage down | local files preserved; upload queue bounded |
| Low memory | shed optional subsystems first; never the capture loop |

## 6. Open architecture questions

- Partition table (two-slot OTA vs single app) — decided with deliverable 5
  once the web bundle size is known.
- Whether the web assets live in the app partition or a separate data
  partition — depends on the same measurement.
