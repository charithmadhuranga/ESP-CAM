---
name: VisionCam reference-first clean-room workflow
description: The mandatory adopt/adapt/defer/reject workflow for studying the two reference projects (s60sc/ESP32-CAM_MJPEG2SD, AGPL-3.0, and espressif/esp32-camera, Apache-2.0) before designing any VisionCam subsystem — including the licence policy, the reference map, how to write clean-room notes, and what must be logged in docs/REFERENCE_ANALYSIS.md. Load before implementing camera init, AVI recording, playback, motion, storage/retention, MQTT, config or logging.
license: MIT
metadata:
  version: "1.0"
  author: visioncam-pro
---

# Reference-first (clean-room) skill

Read `AGENTS.md` rules 31–36 and SPEC §1A before using this skill.

## The two references

| Ref | Project | Role | Licence | Pinned |
|---|---|---|---|---|
| R1 | `s60sc/ESP32-CAM_MJPEG2SD` | Application/behaviour reference; already runs on the owner's board → the **hardware baseline** | **AGPL-3.0** | `abe84d89fc74367c21b644cb5101c9b89fd4bfb7` (`APP_VER 10.10.1`) |
| R2 | `espressif/esp32-camera` | Camera driver, **runtime dependency** | Apache-2.0 | `b3f7fe802f41f17fd7a104d517c53f1f62c8414d` (registry 2.1.9) |

Checkouts live in `reference/`, which is **excluded from the firmware build,
from release archives, and from code search when authoring new code** under
the clean-room option — except for deliberate study.

## Licence policy (SPEC §1A.2) — check this first

**Default: Option A, clean-room.** VisionCam Pro stays under the owner's
chosen licence; R1 is only a behavioural reference.

Allowed without further approval:
- Publicly documented formats and protocols (AVI/RIFF, MJPEG, MQTT, HA
  discovery).
- Reusing ideas, architecture, measured numbers and failure lessons.
- New code written from this specification's behaviour descriptions.

**Not allowed** without an explicit owner decision recorded in
`docs/REFERENCE_ANALYSIS.md`:
- Copy-pasting or mechanically translating R1 functions, tables or macros —
  specifically the 310-byte AVI header byte table and the `frameSizeData`
  table.
- Vendoring R1 source into the firmware build.

Option B (adopt AGPL-3.0 with attribution) requires the owner's decision in
writing. R2 may be used under either option; keep its licence notice in the
release.

## Workflow (7 steps, every subsystem)

1. **Locate it** in the SPEC §1A.8 reference map:

   | VisionCam subsystem | Study in R1 | Study in R2 |
   |---|---|---|
   | Board profile, pins | `camera_pins.h` (`CAMERA_MODEL_AI_THINKER`) | `driver/include/esp_camera.h` |
   | Camera init/settings | `mjpeg2sd.cpp` (`initCam`, `prepCam`) | `esp_camera.c`, `sensor.h`, `Kconfig` |
   | Frame manager | `mjpeg2sd.cpp` (`processFrame`, `captureTask`) | `esp_camera_fb_get/return`, `cam_hal.c` |
   | MJPEG stream | `streamServer.cpp` | README "JPEG HTTP Stream" |
   | Snapshot | `mjpeg2sd.cpp`, `streamServer.cpp` | README "JPEG HTTP Capture" |
   | AVI writer | `avi.cpp`, `mjpeg2sd.cpp` (`saveFrame`, `closeAvi`) | n/a |
   | Playback | `mjpeg2sd.cpp` (`openSDfile`, `getNextFrame`) | n/a |
   | Motion | `motionDetect.cpp` | `conversions/` JPEG helpers |
   | Time lapse | `mjpeg2sd.cpp` (`timeLapse`) | n/a |
   | Storage/retention | `utilsFS.cpp` (`prepSD_MMC`, `checkFreeStorage`) | n/a |
   | HTTP server + WS | `webServer.cpp` | README stream/capture examples |
   | MQTT / HA | `mqtt.cpp` | n/a |
   | Config | `prefs.cpp` | n/a |
   | Logging, crash loop | `utilsLog.cpp` | n/a |

2. **Read** the R1 implementation and the R2 API for it.
3. **Write, in your own words**, what it does, its limits and its failure
   modes. This note — not R1's text — is what you implement from.
4. **Decide** adopt / adapt / defer / reject, checking the licence policy.
5. **Implement** from the specification and your written notes.
6. **Compare** behaviour and numbers against the R1 baseline on hardware.
7. **Record** decision, evidence and any divergence in
   `docs/REFERENCE_ANALYSIS.md`.

If R1 and the specification disagree, **the specification wins** — flag the
difference to the owner.

## Known deliberate divergences (do not "fix" these back to R1)

| Area | R1 | VisionCam |
|---|---|---|
| Framework | Arduino sketch | Native ESP-IDF via PlatformIO |
| Auth | Basic over HTTP, `strcmp` | Sessions, forced password change, salted hash, CSRF |
| UI delivery | Files on SD, downloaded from GitHub | Embedded in firmware, React + TS |
| Playback during capture | Refused | Never stalls recording; else `503 PLAYBACK_BUSY` |
| Crash safety | Header/index only at close | Short segments + provisional header + boot recovery scanner |
| Motion during continuous recording | Disabled | Keeps running so events are marked |
| MQTT topics | `homeassistant/sensor/{hostname}` | `visioncam/{device_id}` stable IDs |
| Images over MQTT | Published on motion | Never (default); authenticated URL instead |
| Config | Key/value file + Preferences, save+reboot | NVS authoritative, versioned, live apply |

## Baseline-first rule

R1 already runs on the owner's board, so it is the measurement bar:

1. Flash R1 unmodified with its own Arduino toolchain (arduino-esp32 ≥ 3.1.1,
   `CAMERA_MODEL_AI_THINKER`) — a **measurement tool only**, never VisionCam's
   build system.
2. Record the baseline table in `docs/HARDWARE.md`: FPS per frame size and
   quality, SD write speed, free/min heap, largest free block, GPIO4/lamp
   behaviour with SD mounted, brownout/reset behaviour, exact SD card model.
3. VisionCam must match within the owner-agreed tolerance (**10 %**,
   decided 2026-11-15) at
   the same frame size, quality and card. Any shortfall is explained and
   recorded, never hidden.
4. Never mark a performance claim verified without its matching baseline row.
5. Treat R1's published numbers as targets until reproduced on this board.

## Arduino → ESP-IDF porting reminders (SPEC §1A.10)

`SD_MMC`/`File` → `esp_vfs_fat_sdmmc_mount` + POSIX I/O · `Preferences` →
`nvs_*` · `WiFi` → `esp_wifi`/`esp_netif`/`esp_event` · NTP → `esp_sntp` ·
TLS → `esp-tls` + cert bundle · `millis()` → `esp_timer_get_time()` ·
`pinMode` → `gpio_config` · `psramFound()`/`ps_malloc()` →
`heap_caps_malloc(MALLOC_CAP_SPIRAM)` · `Serial` → `ESP_LOGx` + RTC RAM ring ·
reset cause → `esp_reset_reason()` · JSON → `cJSON` (bounded).

Rules: no Arduino core without an ADR; do not copy R1's global-variable /
`INCLUDE_*` style; verify C stdio buffering actually reaches the card in
sector-multiple blocks; test GPIO4 behaviour with the **ESP-IDF** SDMMC driver.

## Output format for `docs/REFERENCE_ANALYSIS.md`

```markdown
## <subsystem> — <date>
- **Studied:** R1 `<file>` @ abe84d8, R2 `<file>` @ b3f7fe8
- **What it does / limits / failure modes:** <own words>
- **Decision:** adopt | adapt | defer | reject
- **Reason:** <why, incl. licence check>
- **Divergence from R1:** <or "none">
- **Evidence:** <baseline row, measurement, or "UNVERIFIED">
```
