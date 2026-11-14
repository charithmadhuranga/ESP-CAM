# VisionCam Pro

A standalone, local-first surveillance camera for the **AI-Thinker ESP32-CAM**
(OV2640, microSD, ESP32-CAM-MB), built with PlatformIO on **native ESP-IDF**
(no Arduino layer) and an embedded React + TypeScript web UI.

> **The promise:** the camera keeps capturing, recording and managing local
> media when internet, MQTT, external AI and network storage are all
> unavailable.

**Status:** Phase 0 in progress — repository and platform baseline.
Nothing is verified on hardware yet; every hardware claim in `docs/` is marked
`UNVERIFIED` until a serial log backs it.

---

## Repository layout

| Path | What it is |
|---|---|
| `SPEC.md` | Normative specification (authority #1) |
| `DESIGN.md` | Authoritative UI design, tokens and rules |
| `mockup-ui/` | 17 desktop artboards; `[bracketed]` values are placeholders |
| `reference/` | Pinned R1/R2 checkouts — **behavioural evidence only, never compiled** |
| `firmware/` | ESP-IDF application (`src/`) and `board_profile.h` |
| `webui/` | React + TypeScript + Vite interface |
| `docs/` | HARDWARE, ARCHITECTURE, REFERENCE_ANALYSIS, `adr/` |
| `.opencode/plans/` | Analysis, 10 phase work orders, acceptance matrix |
| `.opencode/skills/` | Agent skills for this project |
| `AGENTS.md` | Mandatory rules for any coding agent (SPEC §14) |

Authority order when documents disagree:
**`SPEC.md`** → **`DESIGN.md`** (UI only) → **`mockup-ui/`** → **`reference/`**
(behaviour only, never code).

## Toolchain

Pinned and recorded in [`docs/adr/0001-toolchain.md`](docs/adr/0001-toolchain.md):

- PlatformIO Core 6.2.0 · `platformio/espressif32@8.0.0`
- **ESP-IDF 6.1.0** (`platformio/framework-espidf@4.60100.0`)
- `framework = espidf` — the Arduino core is **not** part of this build
  (AGENTS rule 37)
- Board `esp32cam`, 4 MB flash, PSRAM + flash at 80 MHz

```bash
pio run                 # build firmware
pio run -t upload       # flash over ESP32-CAM-MB
pio device monitor      # serial at 115200
```

Web UI (before a firmware build that embeds assets):

```bash
cd webui && npm ci && npm run build
```

## Reference projects

| Ref | Project | Licence | Role |
|---|---|---|---|
| R1 | `s60sc/ESP32-CAM_MJPEG2SD` | **AGPL-3.0** | Behavioural reference + hardware baseline only |
| R2 | `espressif/esp32-camera` | Apache-2.0 | The runtime camera driver (pinned) |

**Owner decision D1 (2026-11-15): Option A — clean-room.** R1 source is never
copied, vendored or linked into VisionCam. See `docs/REFERENCE_ANALYSIS.md`
and `reference/README.md`.

```bash
reference/fetch_references.sh --verify   # re-check both pinned trees
```

## Phases

Plan index and gates: [`.opencode/plans/README.md`](.opencode/plans/README.md).

| Phase | Focus | Gate |
|---|---|---|
| 0 | Repository and platform baseline | Flashes, JPEG, SD 1-bit, PSRAM, R1 baseline |
| 1 | Core camera and web UI | Authenticated live view, no internet needed |
| 2 | Storage and snapshots | Snapshots survive reboot, gallery paginates |
| 3 | Recording and playback | AVI validates, interrupted segments recover |
| 4 | Motion and events | Bounded event queue, tunable false triggers |
| 5 | Timelapse | Short project produced without disrupting capture |
| 6 | AI framework | Model rejected/accepted correctly, camera never blocked |
| 7 | MQTT and Home Assistant | Entities appear; broker outage doesn't stop recording |
| 8 | Network storage | Upload after close; failure preserves local files |
| 9 | Hardening and release | 24 h mixed workload, no unexplained resets |

One phase at a time — never start N+1 with a red gate in N.

## Licence

Reference projects retain their own licences (`reference/*/LICENSE`).
VisionCam Pro's own licence is the owner's to state.
