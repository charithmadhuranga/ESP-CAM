# VisionCam Pro

## Coding-Agent-Ready Product, Firmware, Web UI and Integration Specification

**Document type:** Engineering specification and implementation guide\
**Target hardware:** AI-Thinker ESP32-CAM, OV2640 camera, onboard
microSD slot, ESP32-CAM-MB USB programmer\
**Firmware framework:** PlatformIO with `framework = espidf` (native
ESP-IDF, no Arduino layer)\
**Web application:** React + TypeScript, compiled into static assets
embedded in firmware\
**Storage:** microSD primary, optional network storage integration\
**AI:** On-device TensorFlow Lite Micro where feasible, plus optional
external AI inference server\
**Recording:** Continuous and event-based\
**Integrations:** MQTT 3.1.1 and Home Assistant MQTT Discovery\
**Product name:** VisionCam Pro
**Reference projects:** `s60sc/ESP32-CAM_MJPEG2SD` (R1, application
reference, AGPL-3.0) and `espressif/esp32-camera` (R2, camera driver,
Apache-2.0). See Section 1A.\
**Revision:** 2 (adds reference-project analysis and inspired-by
decisions throughout)\

------------------------------------------------------------------------

# 1. Product Definition

VisionCam Pro is a standalone, local-first smart surveillance camera
built around the AI-Thinker ESP32-CAM. It provides its own authenticated
web interface, live video streaming, snapshot management, continuous and
event-based recording, timelapse creation, local storage management,
motion detection, optional on-device TinyML inference, optional external
AI inference, MQTT notifications, and Home Assistant discovery.

The device must remain useful when the internet, MQTT broker, external
AI server, or optional network storage is unavailable. Core camera,
local recording, snapshots, local playback, motion detection, and local
administration must continue on the device.

## 1.1 Product goals

1.  Operate as a self-contained Wi-Fi camera without requiring a PC,
    cloud account, or NVR.
2.  Provide a professional responsive web UI served directly by the
    ESP32-CAM.
3.  Protect all management, stream, media, and API resources with
    single-user authentication.
4.  Store snapshots, recordings, timelapse source frames, and metadata
    on the microSD card.
5.  Support continuous recording and event-triggered recording with
    configurable policies.
6.  Provide browser playback and downloads for supported recording
    formats.
7.  Provide motion detection and a pluggable AI inference architecture.
8.  Support validated TFLite Micro models on the device, subject to RAM,
    flash, operator, and performance constraints.
9.  Optionally forward selected frames to an external AI inference
    service.
10. Publish events and state to a configurable MQTT broker.
11. Implement Home Assistant MQTT Discovery with stable device and
    entity identifiers.
12. Support optional network storage without making it a dependency for
    core operation.
13. Recover safely from Wi-Fi loss, broker loss, external service loss,
    SD-card full conditions, and unexpected resets.
14. Reuse proven designs from the two reference projects (Section 1A)
    instead of reinventing camera handling, AVI recording, playback,
    motion detection, storage management and MQTT basics, and document
    every deliberate divergence.

## 1.2 Non-goals and explicit constraints

The target is the original ESP32 in the AI-Thinker module, not an
ESP32-S3. The design must not assume the performance, RAM, camera
throughput, or AI capability of an ESP32-S3 or Jetson-class device.

The original ESP32-CAM does not provide a hardware H.264 encoder. The
firmware must not promise native MP4/H.264 recording or unrestricted
multi-stream operation.

The system must not claim that arbitrary TensorFlow Lite models will
run. Only models specifically converted and validated for TensorFlow
Lite Micro, supported operators, available tensor arena, and the target
memory budget may be installed.

The ESP32-CAM is not a hardened internet-facing security appliance. Do
not expose its web server directly to the public internet. Remote access
should use a trusted VPN, secure reverse proxy, or a separately secured
gateway.

R1 itself states that the original ESP32 cannot support all of its
features because it runs out of heap. VisionCam Pro therefore adopts R1's
core record/replay/motion/storage/MQTT patterns and deliberately does not
attempt R1's audio, RTSP, telemetry, remote-control, photogrammetry,
intercom, camera-hub or on-device HTTPS features in the first release.

------------------------------------------------------------------------

# 1A. Reference Projects: Analysis, Reuse Policy and Inspired-By Decisions

**Added in Revision 2.** VisionCam Pro must not reinvent subsystems that
two mature projects already solve on this exact board. Before designing
or implementing any subsystem, the coding agent must read this section,
study the mapped reference code (Section 1A.8), and either **adopt**,
**adapt**, **defer** or explicitly **reject** the reference approach with
a written reason in `docs/REFERENCE_ANALYSIS.md`.

## 1A.1 The two reference projects

| Ref | Project | Role for VisionCam Pro | License | Observed version | Pinned commit (analysed) |
|---|---|---|---|---|---|
| **R1** | `s60sc/ESP32-CAM_MJPEG2SD` | Full application reference. Already runs on the product owner's AI-Thinker ESP32-CAM. Source of proven recording, playback, motion, storage and MQTT patterns plus measured performance. | **AGPL-3.0** | `APP_VER 10.10.1` (master, commit date 2026-11-11) | `abe84d89fc74367c21b644cb5101c9b89fd4bfb7` |
| **R2** | `espressif/esp32-camera` | Camera driver reference and **runtime dependency**. Defines frame-buffer semantics, sensor control and JPEG conversion helpers. | **Apache-2.0** | Component Registry `2.1.9` (commit date 2026-11-09) | `b3f7fe802f41f17fd7a104d517c53f1f62c8414d` |

Notes:

- The commits above are what this revision was analysed against. The
  agent must re-verify against whatever version is finally pinned in
  `platformio.ini` and record any behavioural difference.
- The repository's `library.json` for R2 reports version `2.0.0`, while
  the Component Registry reports `2.1.9`. Do not trust a floating
  floating dependency entry. Add R2 as an ESP-IDF component pinned to an
  exact version or commit, and confirm at build time which copy is
  actually linked (two copies must never coexist).
- Evidence labels used below: **[R1-code]** read in source,
  **[R1-readme]** stated in the R1 README, **[R2-code]** read in source
  or Kconfig, **[R2-readme]** stated in the R2 README. Statements about R1
  files that were not read in depth are based on the README and file names
  and must be re-checked before they are relied upon.

## 1A.2 License policy (important)

R1 is licensed AGPL-3.0. Copying or closely porting its source code into
VisionCam Pro makes VisionCam Pro a derivative work and brings AGPL
obligations, including offering source to users who interact with the
device over a network. This is not legal advice; the product owner must
decide.

Allowed by default (**clean-room, inspired-by**):

- Reusing publicly documented formats and protocols (AVI/RIFF layout,
  MJPEG, MQTT, Home Assistant discovery).
- Reusing ideas, architecture, measured numbers, and failure lessons
  documented in this section.
- Writing new code from the behaviour descriptions in this specification.

Not allowed without an explicit owner decision recorded in
`docs/REFERENCE_ANALYSIS.md`:

- Copy-pasting or mechanically translating R1 functions, tables or
  macros (including the 310-byte AVI header byte table and the
  `frameSizeData` table) into VisionCam Pro source.
- Vendoring R1 source into the firmware build.

Owner options: (A) **Clean-room** (default): VisionCam Pro stays under
the owner's chosen license; R1 is only a behavioural reference. (B)
**Adopt AGPL-3.0** for VisionCam Pro and reuse R1 code with attribution.
R2 (Apache-2.0) may be used as a dependency under either option; keep its
license notice in the release.

`reference/` (Section 13) holds a pinned, **non-compiled** checkout script
for R1 and R2. R1 must never be linked into a release build under option
A.

## 1A.3 Analysis of R2: `espressif/esp32-camera`

### What it is

A camera driver for ESP32, ESP32-S2 and ESP32-S3 covering OV2640, OV3660,
OV3640, OV5640, OV7670, OV7725 and several other sensors, plus helpers to
convert frames to JPEG, BMP and RGB888. OV2640 maximum resolution is
1600x1200 [R2-readme].

### Facts the firmware design must respect

| Topic | Verified behaviour | Consequence for VisionCam Pro |
|---|---|---|
| PSRAM | Except for CIF or lower in JPEG mode, the driver requires PSRAM installed and enabled [R2-readme]. | Refuse frame sizes above CIF without PSRAM; report it in the UI. |
| `fb_count = 1` | Driver waits for VSYNC, runs I2S DMA for one frame, stops, then hands the buffer to the application. More control, longer time to get a frame [R2-readme]. | Use for low-rate snapshot/timelapse modes. |
| `fb_count >= 2` | I2S runs continuously; frames are pushed to a queue. Roughly double frame rate, more CPU/memory strain, JPEG only [R2-readme]. | Required for smooth streaming/recording. |
| `grab_mode` | `CAMERA_GRAB_WHEN_EMPTY` fills empty buffers (first frames may be old). `CAMERA_GRAB_LATEST` keeps the most recent `fb_count` frames (not applicable with one buffer) [R2-code]. | Use `GRAB_LATEST` for live/recording; document the stale-frame trade-off. |
| `jpeg_quality` | 0-63; for OV sensors a lower number means higher quality [R2-readme]. | Matches Section 4.1. |
| `jpeg_buffer_size` | 0 selects the default derived from frame size (`width*height/5`, see `cam_hal.c`); the Kconfig help warns that very small sizes such as QQVGA can get an insufficient buffer with the default [R2-code]. | Frame manager must reject frames with `len == 0` or larger than the configured buffer; set an explicit size for tiny frame sizes. |
| `fb_location` | `CAMERA_FB_IN_PSRAM` or DRAM [R2-code]. | PSRAM for frames; keep DRAM for stacks, DMA and network buffers. |
| `esp_camera_fb_get` / `esp_camera_fb_return` | Every acquired buffer must be returned to the driver [R2-readme]. | Matches the single-owner frame manager rule; leak = camera stall. |
| Non-JPEG formats | On ESP32, do not use sizes above QVGA when not JPEG; YUV/RGB strains PSRAM bandwidth and can drop data, especially with Wi-Fi on [R2-readme/code]. | JPEG-only capture. Decode to small grayscale only for motion/AI. |
| Camera task | Pinned to core 0 by default, 4096-byte stack, DMA buffer maximum 32768 bytes by default [R2-code, Kconfig]. | Do not put heavy application tasks on core 0 by accident; measure. |
| Kconfig | Unused sensors can be disabled to save memory [R2-code]. | Disable everything except OV2640 if the build system permits (see caveat below). |
| Conversion helpers | `frame2jpg`, `fmt2jpg`, `fmt2rgb888`, `fmt2bmp` [R2-readme]. | Use for thumbnails/decoding instead of writing converters. |
| Sensor control | `esp_camera_sensor_get()` returns a `sensor_t` with `set_framesize`, `set_vflip`, `set_brightness`, etc. [R2-code]. | Camera settings API is a thin wrapper over this. |

**Build-system note.** VisionCam Pro builds with native ESP-IDF, so it
owns `sdkconfig`. That gives full control over R2's Kconfig menu: enable
only OV2640 support, choose the camera task core, DMA buffer size and
JPEG buffer mode. R2's README also requires PSRAM enabled with flash and
PSRAM at 80 MHz. Option names change between ESP-IDF versions (for
example the PSRAM symbols), so verify every symbol against the pinned
ESP-IDF and record the baseline in `sdkconfig.defaults` and an ADR.

### R2 usage rules for VisionCam Pro

1. R2 is a **dependency, not a reimplementation target**. Never fork or
   copy the driver.
2. Camera setup code follows the R2 `camera_config_t` contract and R1's
   proven initialisation order (Section 4.1.1).
3. The R2 example JPEG HTTP capture and stream handlers are the baseline
   for the MJPEG stream route; VisionCam adds authentication, client
   limits and frame-manager leasing around them.
4. Pin the R2 version and re-run the Phase 0 camera smoke test whenever
   it changes.
5. Consume R2 as an ESP-IDF component. Its README states the ESP-IDF
   instructions also work for PlatformIO when `framework = espidf`
   [R2-readme]; declare it in `idf_component.yml` with an exact version.

## 1A.4 Analysis of R1: `s60sc/ESP32-CAM_MJPEG2SD`

### What it is

An Arduino-framework application (so none of its Arduino API calls can be
used directly in VisionCam Pro; see Section 1A.10) for ESP32 and ESP32-S3 that records JPEG
frames from an OV2640/OV3660/OV5640 camera into AVI files on SD, replays
them to a browser as MJPEG, and supports motion capture, time lapse,
dashcam-style continuous recording, MQTT with Home Assistant, FTP/HTTPS
upload, WebDAV, Telegram/SMTP alerts, RTSP, audio, telemetry and many
peripherals [R1-readme]. It is a large (about 17,000 lines under
`src/`) multi-feature product, not a minimal reference.

### Honest scope note from R1's own README

R1 states that the original ESP32 **cannot support all of its features**
because it runs out of heap, and recommends ESP32-S3 boards [R1-readme].
Specifically: audio recording is not viable on ESP32; telemetry, NVR
streaming and machine learning are better or only feasible on S3; HTTPS
is only usable on S3 and can still be unstable; Telegram uses a lot of
heap because of TLS. VisionCam Pro targets the original ESP32, so it must
copy R1's **core** (record, replay, motion, storage, MQTT) and not its
peripheral breadth.

### Source map (what each R1 area does)

| R1 area | Role | Evidence |
|---|---|---|
| `mjpeg2sd.cpp` | Camera init, capture task, record open/save/close, time lapse, SD playback | R1-code (read) |
| `avi.cpp` | AVI header template, index buffer, header patching, WAV merge | R1-code (read in full) |
| `motionDetect.cpp` | Camera-based motion detection from decoded small grayscale frames; optional Edge Impulse classification hook | R1-code (partly read), R1-readme |
| `streamServer.cpp` | Concurrent stream tasks and per-stream PSRAM buffers | R1-code (partly read) |
| `webServer.cpp` | `esp_http_server` setup, Basic auth, WebSocket, URI handlers | R1-code (read) |
| `utilsFS.cpp` | SD_MMC mount, free-space management, oldest-folder deletion, file listing | R1-code (read) |
| `prefs.cpp` | Configuration (key/value file plus Preferences) | R1-code (partly read) |
| `mqtt.cpp` | `esp-mqtt` client, LWT, Home Assistant integration | R1-code (partly read) |
| `ftp.cpp`, `webDav.cpp`, `telegram.cpp`, `smtp.cpp`, `rtsp.cpp`, `audio.cpp`, `telemetry.cpp`, `peripherals.cpp`, `periphsI2C.cpp`, `wifiEth.cpp`, `utilsLog.cpp`, `setupAssist.cpp`, `externalHeartbeat.cpp` | Upload, remote management, alerts, streaming, audio, sensors, networking, logging, HTTPS setup, heartbeat | File names plus R1-readme only |

### Proven design facts worth inheriting

**Camera initialisation [R1-code].**
- Initialise with the **largest frame size the installed PSRAM can hold**
  (SVGA for about 2 MB, UXGA for about 4 MB, QSXGA for about 8 MB), so
  buffers are allocated once at full size, then switch to the user's
  configured frame size at runtime with `set_framesize`. Default is VGA.
- `fb_count = 4`, `CAMERA_GRAB_LATEST`, PSRAM frame buffers, JPEG quality
  12 at init, 20 MHz XCLK.
- Maximum frame buffer size is computed as `width*height/5`, matching R2's
  default.
- Init is retried twice, power-cycling the camera through the PWDN pin
  between attempts.
- After init the sensor PID is read and checked (OV2640, OV3660, OV5640,
  other); an unknown PID fails startup with a readable message.
- The first few frames are discarded so auto exposure and white balance
  can settle.
- On AI-Thinker the SD driver still touches GPIO4 even in 1-bit mode, so
  R1 explicitly drives GPIO4 low after mounting to keep the lamp off.

**Capture loop [R1-code].**
- One periodic capture task (priority 6, 4 KB stack) uses
  `vTaskDelayUntil`, so it catches up if a cycle runs long.
- A `NULL` frame triggers time-limited recovery, not a tight loop: at most
  one attempt every 2 seconds, up to 10 attempts, using a sensor reset,
  with a 10 ms yield to avoid a task-watchdog reset.
- Frames with length 0 or larger than the buffer are dropped.
- Each active stream gets a **copy** of the frame in its own PSRAM
  buffer, signalled by a semaphore; the camera buffer is returned
  quickly. This is the same "copy for slow consumers, lease for fast
  consumers" idea as the VisionCam frame manager.

**AVI recording [R1-code].**
- A fixed 310-byte header template is patched at close (frame count,
  microseconds per frame, dimensions, sizes). Space for it is reserved at
  the start of the file and the file is rewritten at offset 0 on close.
- Each JPEG is a `00dc` chunk padded to a 4-byte boundary; the `idx1`
  index (16 bytes per frame) is built in PSRAM while recording and
  appended at close.
- Writes go through a RAM buffer that is a **multiple of the SD sector
  size** (8 KB) to minimise and align SD writes.
- The in-progress file has a temporary name (`/current.avi`), and is
  renamed to a name containing date/time, frame size, actual FPS and
  duration when it closes. Recordings shorter than a minimum duration
  (default 5 s) are deleted.
- The header stores the **measured** FPS (frames divided by elapsed time),
  not the requested FPS, so playback speed is correct even if the SD card
  could not keep up.
- Recording stops automatically at a maximum frame count (default 20,000).
- Files are stored in per-day folders; suffixes mark audio, telemetry,
  continuous (`_C`) and time-lapse (`_T`) files.

**Weakness to fix, not copy.** The header and index exist only after a
clean close. A power loss during recording leaves an unplayable
`current.avi`. VisionCam Pro keeps R1's layout but adds short segments
and a recovery scanner (Section 4.6).

**Playback [R1-code].** The browser does not parse AVI. The device reads
the AVI from SD in sector-multiple blocks, finds `00dc` frames, and sends
them to the browser as a timed MJPEG stream at the recorded FPS.
Playback is **refused while a capture is in progress**.

**Motion detection [R1-code, R1-readme].**
- JPEG frames are decoded to a very small grayscale bitmap and compared
  to the previous sample; the downscale acts as noise smoothing.
- Checking is rate-limited: about 5 checks per second while idle; once
  recording, one check every 2 seconds, which also defines the
  post-motion time.
- Motion is **not available above SXGA** because of JPEG decoder
  limitations.
- The same decode yields an ambient light level used for night-time
  logic (lamp, behaviour switching with hysteresis).
- A debug view overlays changed pixels in red for calibration.
- Triggers can be OR-ed: camera motion, PIR, accelerometer, manual
  button, with the reason recorded.
- Optional classification (Edge Impulse library, 96x96 input) is used to
  decide whether to **keep** a motion recording. R1 says ML is only
  feasible on ESP32-S3.

**Storage [R1-code].** `SD_MMC` 1-bit mode is the default on ESP32 because
it is practically as fast as 4-bit there and frees GPIO4 and GPIO12 [R1-
readme]. When free space falls below a threshold the oldest **day folder**
is deleted (optionally uploaded first). If space cannot be made, recording
is disabled.

**Time lapse [R1-code].** Frames are written straight into a time-lapse
AVI as they are captured (no separate generation step), after time is
synchronised, finishing after a configured duration; it shares the same
index/header code with a separate index buffer.

**HTTP and security [R1-code, R1-readme].** `esp_http_server` with
`lru_purge_enable`, `max_open_sockets = HTTP_CLIENTS + MAX_STREAMS`
(`MAX_STREAMS` is 4: web stream, playback/download, NVR, plus others),
4 KB server stack on ESP32 (7 KB on S3), and WebSocket for log/status.
Authentication is HTTP Basic over plain HTTP using a cached encoded
credential compared with `strcmp`. HTTPS exists but is only practical on
S3.

**MQTT [R1-code].** Uses the ESP-IDF `esp-mqtt` client (3.1.1), QoS 1
messages, a retained **Last Will** (QoS 2) that publishes `offline`, a
retained `online` on connect, a command topic, and subscription to Home
Assistant's `homeassistant/status` birth topic. It publishes a camera
image on motion for HA's MQTT camera. README: connection starts after a
ping to the broker succeeds.

**Configuration and logging [R1-code, R1-readme].** Settings are held in a
key/value file loaded into memory and mirrored with Preferences;
passwords are kept out of the config file; a version key detects obsolete
config files. Log output can go to a 7 KB **RTC RAM ring buffer**
(default, survives resets), WebSocket, or SD. A "crash loop detected"
warning exists and the README states it is usually an inadequate power
supply.

**Task and memory numbers [R1-code].** Priorities: capture 6, stream and
HTTP 5, playback 4, telemetry 3, Telegram/email 1. Stacks (ESP32):
capture 4 KB, HTTP 4 KB, MQTT 4 KB, playback 2 KB, first stream 5 KB.
Warning thresholds: free heap below 32 KB, largest free block below
16 KB; TLS needs about 64 KB of free heap. Several tasks are created with
stacks placed in PSRAM via `xTaskCreateWithCaps`.

### R1 measured performance (vendor-reported)

AI-Thinker OV2640, fresh Sandisk 4 GB SDHC Class 2, maximum JPEG quality,
20 MHz clock [R1-readme]. These are **targets to compare against, not
guarantees**; Phase 0 must re-measure on the owner's board and card.

| Frame size | Camera max fps | R1 recorded fps | Motion detect time (ms) |
|---|---|---|---|
| 96x96 | 50 | 45 | 15 |
| QVGA | 50 | 40 | 70 |
| CIF | 50 | 40 | 110 |
| VGA | 25 | 20 | 80 |
| SVGA | 25 | 20 | 120 |
| XGA | 12.5 | 5 | 180 |
| HD | 12.5 | 5 | 220 |
| SXGA | 12.5 | 5 | 300 |
| UXGA | 12.5 | 5 | 450 |

R1 also reports that a no-name Class 6 card was about 3 times slower
than a genuine Class 2 card, so card quality is a measured variable.

### R1 limitations VisionCam Pro must be aware of

- Not all features fit on the original ESP32 (above).
- No crash-safe recording (header and index only at close).
- Playback unavailable during capture; motion detection is disabled while
  continuous recording runs; time lapse and dashcam modes are mutually
  exclusive.
- Time lapse rides on the capture loop at full FPS, so the camera keeps
  running even for one frame per minute.
- Many settings need "Save" plus reboot.
- Basic auth over HTTP only; no sessions, no CSRF protection.
- Web UI files live on SD and are downloaded from GitHub on first run.
- Browser features are stated as fully tested only on Chrome.
- I did not find pre-event buffering in the parts of R1 that were read.

## 1A.5 Adopt / Adapt / Defer / Reject matrix

| Area | R1 / R2 approach | Decision | VisionCam Pro position |
|---|---|---|---|
| Camera driver | R2 `esp_camera` | **Adopt** (dependency) | ESP-IDF component, pinned, never forked |
| Framework | R1: Arduino sketch | **Reject** | Native ESP-IDF via PlatformIO; port ideas using Section 1A.10 |
| Camera init order | R1: max-size alloc, then runtime resize, retries, PID check, warm-up frames | **Adapt** | Section 4.1.1 |
| Frame buffers | R1: 4 x PSRAM, `GRAB_LATEST` | **Adapt** | Start at 2, validate up to 4 |
| Frame fan-out | R1: copy to per-stream PSRAM buffers | **Adapt** | Frame manager leases plus bounded copies |
| NULL-frame recovery | R1: time-limited, capped, sensor reset | **Adopt idea** | Section 9 |
| AVI container | R1: header template, `00dc`, `idx1` in PSRAM, 4-byte padding | **Adapt** | Same layout, new code, plus crash recovery |
| SD write path | R1: sector-multiple buffer, temp name then rename | **Adopt idea** | Section 4.6 |
| Header FPS | R1: measured FPS | **Adopt idea** | Mandatory |
| Playback | R1: server-side timed MJPEG replay | **Adopt idea** | Primary path; JS AVI parser demoted to optional |
| Playback during record | R1: refused | **Reject** | Must not stop recording; allow read-only playback when headroom allows, otherwise return a clear "busy" |
| Motion | R1: small grayscale, N-of-frames, hysteresis, debug overlay | **Adapt** | Section 4.3.1 |
| ML | R1: Edge Impulse, S3-only | **Defer** | External AI first; TFLM experimental on ESP32 |
| Time lapse | R1: direct-to-AVI | **Adapt** | Default direct-to-AVI plus optional frames (Section 4.7) |
| Free space | R1: delete oldest day folder | **Adapt** | Retention manager with category policy and preview |
| SD mode | R1: 1-bit default | **Adopt** | Matches Section 2.3 |
| Upload | R1: FTP/HTTPS completed files | **Adapt** | HTTP upload backend first; FTP deferred |
| WebDAV | R1: included | **Defer** | Post-v1 convenience only |
| MQTT client | R1: `esp-mqtt` | **Adopt** (library choice) | Section 4.10 |
| MQTT topics | R1: `homeassistant/sensor/{host}/...` | **Reject** | `visioncam/{device_id}/...` with stable IDs |
| Image over MQTT | R1: publishes image on motion | **Reject (default)** | Section 4.10 |
| HA discovery | R1: camera plus motion entities | **Adapt** | Granular entities, stable IDs, cleanup |
| Auth | R1: Basic over HTTP | **Reject** | Sessions, forced password change, CSRF |
| TLS on device | R1: S3 only | **Defer** | Not a v1 deliverable on ESP32 |
| UI delivery | R1: SD plus GitHub download | **Reject** | Embedded in firmware |
| UI technology | R1: static htm/js | **Reject** | React + TypeScript |
| Config store | R1: key/value file + Preferences | **Adapt** | NVS authoritative, versioned, SD backup |
| Logging | R1: RTC RAM ring, WS, SD | **Adopt idea** | Section 8.4 |
| Audio, RTSP, telemetry, servos, RC, photogrammetry, intercom, hub | R1 features | **Defer / out of scope** | Not on original ESP32 v1 |
| Telegram / SMTP | R1 alerts | **Defer** | TLS heap cost; optional later integration |

## 1A.6 What is genuinely new in VisionCam Pro

These have no equivalent in R1, so they are designed from this
specification and must not be "found" in R1: authenticated session UI
with forced credential change; opaque media IDs and JSONL media/event
indexes; React web application; event model and event bus; MQTT topic
schema and granular Home Assistant entities; external AI provider
abstraction; retention manager with preview; segment crash recovery.

## 1A.7 Baseline-first rule (R1 already runs on the target board)

Because R1 is known to run on the owner's board, it is the hardware
baseline:

1. Flash R1 unmodified using R1's own Arduino toolchain (arduino-esp32
   core 3.1.1 or newer) with `CAMERA_MODEL_AI_THINKER`. This is a
   **measurement tool only**; it is not VisionCam Pro's build system.
2. Record the baseline table in `docs/HARDWARE.md`: recorded FPS per frame
   size and JPEG quality, SD write speed, free heap, minimum free heap,
   largest free block, GPIO4/lamp behaviour with SD mounted, brownout and
   reset behaviour, and the exact SD card model.
3. Keep that data as the acceptance bar. VisionCam Pro recording FPS at
   the same frame size, quality and card should match the R1 baseline
   within a tolerance agreed with the owner (suggested 10 percent). Any
   shortfall must be explained and recorded, not hidden.
4. Never mark a VisionCam performance claim as verified without the
   matching baseline row.

## 1A.8 Reference map: where to look first

| VisionCam subsystem | Study in R1 | Study in R2 |
|---|---|---|
| Board profile, pins | `camera_pins.h` (`CAMERA_MODEL_AI_THINKER`) | `driver/include/esp_camera.h` |
| Camera init and settings | `mjpeg2sd.cpp` (`initCam`, `prepCam`) | `esp_camera.c`, `sensor.h`, `Kconfig` |
| Frame manager | `mjpeg2sd.cpp` (`processFrame`, `captureTask`) | `esp_camera_fb_get/return`, `cam_hal.c` |
| MJPEG stream | `streamServer.cpp` | README "JPEG HTTP Stream" example |
| Snapshot | `mjpeg2sd.cpp`, `streamServer.cpp` | README "JPEG HTTP Capture" example |
| AVI writer | `avi.cpp`, `mjpeg2sd.cpp` (`saveFrame`, `closeAvi`) | n/a |
| Playback | `mjpeg2sd.cpp` (`openSDfile`, `getNextFrame`) | n/a |
| Motion | `motionDetect.cpp` | `conversions/` JPEG helpers |
| Time lapse | `mjpeg2sd.cpp` (`timeLapse`) | n/a |
| Storage and retention | `utilsFS.cpp` (`prepSD_MMC`, `checkFreeStorage`) | n/a |
| HTTP server and WS | `webServer.cpp` | README stream/capture examples |
| MQTT / HA | `mqtt.cpp` | n/a |
| Config | `prefs.cpp` | n/a |
| Logging, crash loop | `utilsLog.cpp` | n/a |
| Upload | `ftp.cpp` | n/a |

## 1A.9 Reference-conformance rules (summary)

1. Read the mapped reference code before designing each subsystem.
2. Record adopt/adapt/defer/reject and the reason.
3. Do not copy R1 code under the clean-room option (Section 1A.2).
4. Treat R1 numbers as targets until Phase 0 reproduces them.
5. Prefer an R2 API over writing a converter, a sensor driver, or
   register code.
6. Where VisionCam Pro deliberately differs from R1 for security,
   crash-safety or architecture reasons, keep the difference and document
   it.

## 1A.10 Porting R1 ideas from Arduino to ESP-IDF

VisionCam Pro is built with PlatformIO and native ESP-IDF, so every R1
Arduino API must be replaced by its ESP-IDF equivalent. The mapping below
is a starting point; **verify each API against the pinned ESP-IDF
version** before use.

| R1 (Arduino) use | ESP-IDF replacement in VisionCam Pro |
|---|---|
| `SD_MMC.begin(...)`, `File` | `esp_vfs_fat_sdmmc_mount()` with `sdmmc_host_t` (1-bit) and `sdmmc_slot_config_t` (width 1); POSIX `fopen/fread/fwrite/rename/remove/stat/mkdir/opendir`; `fsync` where durability matters |
| `Preferences` | `nvs_flash` / `nvs_*` API |
| `WiFi.h`, `WiFiClient` | `esp_wifi`, `esp_netif`, `esp_event` |
| NTP / mDNS | `esp_sntp`, `mdns` component |
| `WiFiClientSecure` / TLS | `esp-tls`, certificate bundle (`esp_crt_bundle`) |
| `millis()`, `delay()` | `esp_timer_get_time()`, `vTaskDelay(pdMS_TO_TICKS(n))` |
| `pinMode`, `digitalWrite` | `gpio_config`, `gpio_set_level` |
| `psramFound()`, `ps_malloc()` | `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`, `esp_psram_get_size()` (name varies by IDF version) |
| `xTaskCreateWithCaps` | Same API in ESP-IDF's FreeRTOS; check required config options for PSRAM stacks |
| `Serial` logging | `ESP_LOGx` with log levels; RTC RAM ring buffer in `RTC_NOINIT_ATTR` memory |
| Reset cause | `esp_reset_reason()` |
| OTA | `esp_ota_ops` / `esp_https_ota` with a two-slot partition table |
| JSON | `cJSON` (bundled with ESP-IDF); bound sizes, check heap |
| `esp_jpg_decode` for motion | Provided by R2's conversion code or ESP-IDF's JPEG decoder component; confirm which one is linked |
| `esp_http_server`, `httpd_ws`, `esp-mqtt` | Already ESP-IDF APIs; reuse the concepts unchanged |

Porting rules:

1. Do not add the Arduino core as an ESP-IDF component without a recorded
   owner decision and an ADR.
2. R1's style (global variables, `INCLUDE_*` macros, one shared globals
   header) must not be copied. Use ESP-IDF components with explicit
   interfaces, as in Section 3 and Section 13.
3. Be careful with C stdio buffering on the SD card: confirm that writes
   reach the card in whole, sector-multiple blocks (R1 relies on aligned
   8 KB writes), using `setvbuf`, large `fwrite` blocks, or lower-level
   file APIs, and measure.
4. GPIO4 behaviour with the IDF SDMMC driver in 1-bit mode must be tested;
   R1's Arduino workaround may or may not be needed.

------------------------------------------------------------------------

# 2. Target Hardware and Board Profile

## 2.1 Required hardware

-   AI-Thinker ESP32-CAM module.
-   OV2640 camera module.
-   ESP32-CAM-MB USB programmer for flashing and USB power.
-   microSD card, preferably a reputable high-endurance card.
-   Stable regulated 5 V supply with sufficient transient current
    capability.
-   2.4 GHz Wi-Fi access point.

Optional: - External edge AI server or NAS. - External MQTT broker. -
Home Assistant instance. - Secure VPN or reverse proxy. - External
regulated power supply for deployment.

## 2.2 Board pin mapping

Use the AI-Thinker camera pin map. Keep this mapping in one
board-profile header and do not duplicate pin constants throughout the
codebase.

  Signal        GPIO
  ----------- ------
  PWDN            32
  RESET           -1
  XCLK             0
  SIOD            26
  SIOC            27
  Y9              35
  Y8              34
  Y7              39
  Y6              36
  Y5              21
  Y4              19
  Y3              18
  Y2               5
  VSYNC           25
  HREF            23
  PCLK            22
  Flash LED        4

**Pin map cross-check.** The table above was compared against R1's
`camera_pins.h` entry for `CAMERA_MODEL_AI_THINKER` and matches
(PWDN 32, XCLK 0, SIOD 26, SIOC 27, Y9..Y2 = 35, 34, 39, 36, 21, 19, 18,
5, VSYNC 25, HREF 23, PCLK 22). R1 lists GPIO4 as the flash LED and GPIO33
as the red signal LED. The R2 README example uses a **WROVER-KIT** pin
map; never copy pins from that example.

GPIO4 is shared with the microSD interface on this board. In the initial
supported configuration, use the onboard microSD in 1-bit SDMMC mode
and document the flash LED/SD pin interaction. Do not assume the flash
LED can be driven freely while the SD card is active. Test the selected
board revision.

## 2.3 SD card interface

Use the onboard SD card through the ESP-IDF SDMMC host and FATFS VFS in
1-bit mode as the initial supported configuration. The exact API and
configuration fields must be checked against the pinned ESP-IDF version.

Example starting point (verify against the pinned ESP-IDF):

``` cpp
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

esp_err_t mountSdCard(sdmmc_card_t** out_card) {
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;   // never auto-format
    mount_cfg.max_files = 5;
    mount_cfg.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;          // 1-bit mode
    // host.max_freq_khz: set deliberately after measurement

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;

    return esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount_cfg, out_card);
}
```

Do not format a card automatically. On mount failure, show a clear
error, preserve all configuration, disable storage-dependent features
safely, and continue to provide diagnostic access where possible.

**Reference findings (R1).** R1 mounts `SD_MMC` in 1-bit mode by default
because on the original ESP32 it is practically as fast as 4-bit mode and
frees GPIO4 and GPIO12. On AI-Thinker boards R1 still drives GPIO4 low
after mounting because the SD driver initialises that pin even in 1-bit
mode, otherwise the lamp can light. VisionCam Pro must do the same and
verify it on the exact board with the ESP-IDF SDMMC driver (Phase 0),
since R1's workaround was written for the Arduino `SD_MMC` library. Keep
`format_if_mount_failed` false; R1 exposes it as a parameter, VisionCam
Pro never enables it.

## 2.4 Power and reliability

-   Use a stable 5 V supply appropriate for Wi-Fi transmit peaks and
    camera operation.
-   Detect and log brownout/reset causes.
-   Avoid powering additional peripherals from GPIO pins.
-   Do not write to the SD card from interrupt handlers.
-   Ensure recording segments are finalized and closed regularly.
-   Use a bounded write queue and explicit backpressure behavior.
-   If storage writes cannot keep up, reduce capture rate or stop
    recording safely rather than exhausting RAM.

------------------------------------------------------------------------

# 3. System Architecture

## 3.1 High-level architecture

``` text
                    2.4 GHz Wi-Fi LAN
                           |
        +------------------+--------------------+
        |                  |                    |
   Browser / Phone     MQTT Broker        Optional AI Server
        |                  |                    |
        | HTTP(S)          | MQTT               | HTTP(S)
        v                  v                    v
+-----------------------------------------------------------+
|                    VISIONCAM PRO                          |
|                                                           |
|  +------------------- ESP32-CAM ------------------------+ |
|  |                                                      | |
|  |  OV2640 -> Camera Frame Manager                      | |
|  |                 |                                    | |
|  |       +---------+----------+                         | |
|  |       |         |          |                         | |
|  |    MJPEG     Motion      AI Dispatcher               | |
|  |    Stream    Engine        |                         | |
|  |       |         |      +----+-----+                   | |
|  |       |         |      |          |                   | |
|  |       |      Events  TFLM      External AI            | |
|  |       |         |   Engine     Client                 | |
|  |       +---------+-----+----------+                   | |
|  |                         |                            | |
|  |                   Event Bus                          | |
|  |                         |                            | |
|  |       +-----------------+-------------------+        | |
|  |       |                 |                   |        | |
|  |   Recorder          Snapshot Manager      MQTT       | |
|  |       |                 |                Publisher    | |
|  |       +-----------------+-------------------+        | |
|  |                         |                            | |
|  |                 Storage Manager                      | |
|  |                         |                            | |
|  |                  microSD Card                         | |
|  |                                                      | |
|  |  HTTP API + Auth + Embedded React Static Assets      | |
|  +------------------------------------------------------+ |
+-----------------------------------------------------------+
```

## 3.2 Architectural principles

-   Local-first: the camera must not depend on a cloud service for core
    operation.
-   Modular firmware: each subsystem has a defined interface and owns
    its resources.
-   Single ownership: one storage manager owns filesystem operations;
    one frame manager owns camera frame acquisition and release.
-   Bounded memory: all queues, buffers, and client counts must have
    explicit limits.
-   Backpressure: producers must react safely when consumers cannot keep
    up.
-   Degraded operation: optional integration failures must not stop
    camera operation.
-   Versioned contracts: API payloads, configuration, MQTT schemas, and
    index formats must carry versions.
-   Observable behavior: expose health, error counters, free heap,
    PSRAM, SD status, task status, and integration status.

## 3.3 Recommended technology choices

  -----------------------------------------------------------------------
  Area                                Choice
  ----------------------------------- -----------------------------------
  Firmware language                   C++

  Build system                        PlatformIO

  Framework                           ESP-IDF (PlatformIO `framework = espidf`)

  RTOS                                FreeRTOS provided by ESP-IDF

  Camera                              Espressif `esp32-camera` IDF component

  Wi-Fi                               ESP-IDF `esp_wifi`, `esp_netif`, `esp_event`

  HTTP                                ESP-IDF `esp_http_server`

  MQTT                                ESP-IDF `esp-mqtt` (MQTT 3.1.1)

  AI                                  TensorFlow Lite Micro, optional

  SD                                  ESP-IDF SDMMC host, FATFS VFS, 1-bit

  Configuration                       NVS (`nvs_flash`)

  Web UI                              React + TypeScript

  UI build                            Vite

  UI delivery                         Embedded gzip/static assets in
                                      firmware

  API format                          JSON

  Media                               JPEG, MJPEG AVI, JPEG-sequence
                                      timelapse

  Optional network storage            HTTP upload/WebDAV/SFTP gateway or
                                      NAS integration through a defined
                                      adapter
  -----------------------------------------------------------------------

### 3.3.1 Decisions derived from the reference projects

- **Camera:** Espressif `esp32-camera` (R2) as a pinned ESP-IDF
  component; make sure only one copy is linked.
- **HTTP:** ESP-IDF `esp_http_server`. Both R1 and the R2 examples use it,
  including WebSocket support (`httpd_ws`). Configure
  `lru_purge_enable = true` and `max_open_sockets` as a function of
  allowed HTTP clients plus stream slots (R1 uses `HTTP_CLIENTS +
  MAX_STREAMS`).
- **MQTT:** ESP-IDF `esp-mqtt` client (as R1), MQTT 3.1.1, rather than a
  second, unrelated MQTT library.
- **Build system:** PlatformIO with `framework = espidf`. Pin the
  PlatformIO platform version and the ESP-IDF version it provides, keep
  `sdkconfig.defaults` under version control, and record both in an ADR.
  Board selection: start from PlatformIO's AI-Thinker ESP32-CAM board
  definition and verify its flash size, partition table and PSRAM settings
  against the physical module. R1's Arduino requirement (core 3.1.1 or
  newer) does not apply to VisionCam Pro.

Do not mix multiple HTTP server libraries without a clear reason. Select
one server implementation, pin its version, and verify simultaneous API
and MJPEG stream behavior early.

------------------------------------------------------------------------

# 4. Functional Requirements

## 4.1 Camera and live view

-   Initialize OV2640 with AI-Thinker pin mapping.
-   Detect PSRAM and select camera frame buffer settings accordingly.
-   Support JPEG capture.
-   Provide configurable frame size, JPEG quality, brightness, contrast,
    saturation, and vertical/horizontal flip where supported by sensor
    driver.
-   Provide live MJPEG stream.
-   Provide single-frame capture endpoint.
-   Expose camera health and frame statistics.
-   Limit concurrent stream clients; default maximum one client on the
    original ESP32.
-   Provide stream timeout and disconnect cleanup.
-   Keep streaming and recording consumers from independently acquiring
    unmanaged camera frame buffers.

Initial defaults: - JPEG pixel format. - VGA (640x480) default live
stream. - JPEG quality 12 as a starting point; document that quality
values are driver-specific and lower values generally mean higher
quality in the Espressif driver. - PSRAM frame buffers when detected. -
Two frame buffers only after stability testing. - Latest-frame grab mode
when supported and appropriate.

The application must adapt to memory pressure by reducing
stream/processing load, not by silently allocating unbounded buffers.

## 4.1.1 Camera driver usage rules (from R1 and R2)

This subsection supersedes the earlier default "Two frame buffers only
after stability testing".

1. **Initialise once at full size.** Detect PSRAM size, pick the largest
   supported frame size for it (R1: about 2 MB: SVGA, about 4 MB: UXGA),
   call `esp_camera_init` with that size, then apply the user's frame size
   with `set_framesize`. Default runtime size is VGA.
2. **Buffers.** Start with `fb_count = 2`, PSRAM frame buffers, and
   `CAMERA_GRAB_LATEST`. R1 runs 4 buffers; allow up to 4 only after the
   Phase 0 stability and heap measurements pass. With `fb_count = 1`
   (R2 on-demand mode) only low-rate snapshot/timelapse modes are allowed.
3. **Clock.** Default XCLK 20 MHz. Do not exceed it on the original ESP32
   without measurement.
4. **Validation.** Read the sensor PID after init and require OV2640;
   fail with a readable message otherwise.
5. **Retry.** If init fails, retry up to twice, power-cycling the camera
   through the PWDN pin (GPIO32) between attempts.
6. **Warm-up.** Discard the first 3 or more frames so auto exposure and
   white balance settle; the motion engine ignores them too.
7. **Frame sanity.** Drop any frame with `len == 0` or `len` greater than
   the configured buffer size, and count it.
8. **Buffer size.** The default JPEG buffer is `width*height/5`; set
   `jpeg_buffer_size` explicitly for very small frame sizes.
9. **Slow consumers.** Copy into a bounded per-consumer PSRAM buffer
   (R1 pattern) or use a reference-counted lease; never hold a camera
   buffer across SD or network I/O.
10. **Core placement.** R2's camera task defaults to core 0. Pin
    application tasks deliberately and measure.
11. **sdkconfig baseline.** PSRAM enabled, flash and PSRAM at 80 MHz (R2
    README), only OV2640 support enabled in the camera menu, explicit
    camera task core, DMA buffer size and JPEG buffer mode. Verify the
    symbol names in the pinned ESP-IDF.

## 4.2 Authentication

Single-user authentication only.

Initial credentials: - Username: `admin` - Password: `admin`

The first successful login must require the user to change the default
password before accessing the rest of the application.

Requirements: - Change username. - Change password. - Current password
verification for credential changes. - Logout. - Session expiration. -
Session revocation after password change. - Login attempt throttling. -
Authentication on all management APIs. - Authentication on stream,
snapshot, recording, and download routes. - CSRF protection for
cookie-authenticated state-changing requests. - No credentials or
session tokens in URL query parameters. - Never log passwords, password
hashes, or session tokens.

Password storage: - Use a vetted password hashing algorithm suitable for
the available target resources. - Prefer a memory-hard KDF only if
memory/time costs can be benchmarked safely on the ESP32. - Otherwise
use a cryptographically strong, salted password KDF supported by the
platform and document its parameters. - Never store plaintext
passwords. - Generate session tokens using the platform cryptographic
random source. - Store only a hash of session tokens if practical.

Session design: - Use an HTTP-only cookie. - Set SameSite
appropriately. - Set Secure when HTTPS is used. - Use a random session
token with bounded lifetime. - Store session state in RAM; restart
invalidates all sessions. - Require reauthentication after reboot. -
Apply authentication to the MJPEG endpoint. Do not embed long-lived
credentials in the stream URL.

Security caveat: The embedded web server should be LAN-only by default.
For deployment across untrusted networks, use a secure VPN/reverse proxy
or a separately reviewed HTTPS implementation. Do not advertise plain
HTTP as internet-safe.

## 4.3 Motion detection

Implement a lightweight motion engine independent of AI.

Requirements: - Configurable enable/disable. - Configurable sample
interval. - Configurable sensitivity/threshold. - Configurable minimum
changed-pixel ratio. - Configurable cooldown. - Optional
region-of-interest mask in a future iteration. - Ignore initial frames
while exposure settles. - Avoid triggering on tiny sensor noise. -
Publish a motion-start event and optional motion-end event. - Trigger
event recording and snapshots according to policy. - Show motion state
in UI and MQTT.

Implementation recommendation: - Use a downsampled grayscale
representation or a low-resolution frame. - Compare sampled pixel
intensities against a previous frame. - Apply a threshold per pixel. -
Compute changed-pixel ratio. - Apply temporal smoothing and cooldown. -
Avoid full-resolution frame copies where possible. - Benchmark CPU and
RAM usage alongside streaming.

Motion detection is not object recognition. UI must label it clearly.

### 4.3.1 Reference-derived motion design (from R1)

- Decode JPEG to a very small grayscale bitmap using R2/IDF JPEG helpers
  (no full-resolution RGB buffers) and compare it with the previous
  sample; the downscale is the noise filter.
- Use **two sample rates**: a higher rate while idle (R1: about 5 checks
  per second) and a lower one while a recording is active (R1: one check
  every 2 seconds), which also defines the post-motion hold time.
  Expose both as configuration.
- Motion detection is **not offered above SXGA** (decoder limits, R1). The
  UI must disable the option and say why.
- Derive an ambient **light level** from the same decode and use hysteresis
  with a settle counter for night-mode decisions.
- Provide a **debug view** that shows changed pixels highlighted, for
  threshold calibration.
- Support a bitmask of trigger sources (camera, manual, future PIR) so
  events record why they started.
- Support a **minimum frames/duration** rule: recordings shorter than the
  minimum are deleted (R1 default 5 s).
- Allow continuous recording and motion detection to run together. R1
  disables motion while continuous recording runs; VisionCam Pro should
  keep detecting so events can still be marked.

## 4.4 AI inference architecture

Implement a provider abstraction:

``` cpp
class IInferenceProvider {
public:
    virtual bool begin() = 0;
    virtual bool isReady() const = 0;
    virtual InferenceResult infer(const FrameView& frame) = 0;
    virtual void stop() = 0;
    virtual const char* providerId() const = 0;
    virtual ~IInferenceProvider() = default;
};
```

Providers: 1. `TfliteMicroProvider`: optional on-device inference. 2.
`ExternalAiProvider`: optional remote inference service. 3.
`NoAiProvider`: default fallback when no model/provider is enabled.

**Reference finding (R1 machine learning).** R1's ML hook uses Edge Impulse
models (96x96 grayscale or RGB input) to decide whether a motion recording
is worth keeping, and R1 states this is only feasible on ESP32-S3. For
the original ESP32 this specification therefore treats **external AI as
the primary path** and on-device TFLite Micro as **experimental**, usable
only for a tiny model that passes the benchmark in Phase 6. Adopt R1's
*usage pattern* (classify after motion to verify/keep an event) rather
than classifying every frame.

### 4.4.1 TFLite Micro

-   Use TensorFlow Lite Micro only.
-   Support a deliberately small, quantized model.
-   Model must be compiled or packaged in a supported format.
-   Do not assume arbitrary model upload is safe.
-   Validate model header, length, checksum, supported operators, input
    tensor shape, output tensor shape, quantization, and tensor arena
    requirement.
-   Use a fixed or bounded tensor arena.
-   Benchmark inference duration, peak memory, and frame impact.
-   Run at a configurable low inference rate.
-   Make AI task lower priority than core streaming and storage safety.
-   Provide model activate/deactivate.
-   Maintain rollback to last known-good model.
-   Do not overwrite active model during upload.
-   Provide model compatibility status and validation error.

Model metadata manifest:

``` json
{
  "schema_version": 1,
  "model_id": "tiny_person_detector_v1",
  "display_name": "Tiny Person Detector",
  "format": "tflite_micro",
  "file": "tiny_person_detector_v1.tflite",
  "sha256": "REPLACE_WITH_SHA256",
  "input": {
    "width": 96,
    "height": 96,
    "channels": 1,
    "dtype": "int8",
    "layout": "NHWC"
  },
  "output": {
    "dtype": "int8",
    "description": "Model-specific output decoder required"
  },
  "required_tensor_arena_bytes": 65536,
  "operators": ["CONV_2D", "DEPTHWISE_CONV_2D", "AVERAGE_POOL_2D", "FULLY_CONNECTED"],
  "classes": ["person"],
  "preprocessing": "grayscale_resize_normalize_v1"
}
```

This is an illustrative manifest, not a promise that the example model
fits. Actual model operator list, tensor arena, decoder, input format,
and output format must be measured and documented.

### 4.4.2 External AI server

External AI is optional and must not block core camera operation.

Support a configurable HTTP(S) endpoint: - Base URL. - Authentication
token or API key. - Request timeout. - Upload resolution. - JPEG
quality. - Inference interval. - Selected event types. - Retry policy. -
TLS verification setting; certificate validation should be enabled for
production. - Maximum upload size.

Recommended protocol:

`POST /api/v1/infer`

Request: `multipart/form-data` with JPEG image and metadata, or JSON
metadata plus binary body if simpler for the selected server.

Example response:

``` json
{
  "request_id": "evt-000123",
  "model": "yolo-small-server",
  "inference_ms": 83,
  "detections": [
    {
      "class_id": 0,
      "label": "person",
      "confidence": 0.94,
      "bbox": {
        "x": 0.12,
        "y": 0.08,
        "width": 0.31,
        "height": 0.72
      }
    }
  ]
}
```

The server may use a larger model such as a YOLO-family model. That
inference runs on the external server, not on the original ESP32-CAM.

External AI policies: - `disabled` - `motion_only` - `periodic` -
`on_device_event` - `manual_snapshot`

If the external server is offline, queue only a small bounded number of
requests or drop them according to policy. Never accumulate images
indefinitely in RAM.

### 4.4.3 AI event normalization

Normalize detections from every provider into one common structure:

``` json
{
  "event_id": "evt-000123",
  "source": "external_ai",
  "class_id": 0,
  "label": "person",
  "confidence": 0.94,
  "bbox": [0.12, 0.08, 0.43, 0.80],
  "timestamp": "2026-11-15T10:30:00Z",
  "snapshot_id": "snap-000123"
}
```

Coordinates are normalized from 0.0 to 1.0. Define whether bbox values
represent x/y/width/height and keep it consistent.

## 4.5 Snapshot system

Snapshot sources: - Manual capture. - Motion event. - On-device AI
detection. - External AI detection. - Timelapse source frame.

Requirements: - JPEG capture. - Timestamped filenames. - Event
association. - Thumbnail generation where practical. - Metadata index. -
Download. - Delete. - Gallery filters. - Full-screen image viewer. -
Pagination. - Configurable image quality and capture resolution. - Avoid
storing duplicate snapshots for repeated detections within cooldown.

Suggested path:

``` text
/snapshots/YYYY/MM/DD/{manual,motion,detection,timelapse}/
```

## 4.6 Recording engine

Support: - Continuous recording. - Event-based recording. - Manual
recording. - Configurable recording schedule. - Configurable resolution
and frame rate. - Configurable segment duration. - Configurable event
cooldown. - Optional pre-event buffer. - Configurable post-event
duration. - Recording status and health. - Playback and download.

### Recording formats

The original ESP32-CAM has no hardware H.264 encoder. Use JPEG-based
recording.

Recommended initial format: - MJPEG AVI segments containing JPEG
frames. - JPEG sequence for timelapse source. - Browser-side custom JPEG
sequence player for playback. - Download original AVI segments.

Do not promise universal native AVI playback in browsers. Browser
support varies. The application should implement a custom player that
parses the supported AVI/MJPEG structure or uses a JPEG frame sequence
index.

MP4/H.264 conversion is a future external-server capability.

### Reference-derived AVI writer design (baseline for implementation)

VisionCam Pro implements the same on-disk structure R1 proves works on
this board, written as new code (see license policy, Section 1A.2).

1. **Container.** RIFF AVI with a fixed-size header region reserved at the
   start of the file, `movi` list of `00dc` JPEG chunks, each padded to a
   4-byte boundary, and a trailing `idx1` index of 16 bytes per frame
   (chunk id, flags, offset, size). The AVI header is documented RIFF
   structure; derive it from the public AVI format, not from R1's byte
   table.
2. **Index in PSRAM.** Build the index in a PSRAM buffer sized for the
   segment's maximum frame count (R1: `(maxFrames + 2) * 16` bytes) and
   append it at close. Bound the frame count per segment so the index
   buffer cannot overflow; rotate the segment instead of rebooting.
3. **Sector-aligned writes.** Stage frames in a RAM buffer whose size is
   a multiple of the SD sector size (R1: 8 KB) and write in whole buffers.
   Never write each JPEG as many small writes.
4. **Temporary name.** Write to a temporary in-progress name and rename
   to the final name only after the header and index are written.
5. **Measured FPS.** Store actual frames divided by elapsed time in the
   header, not the requested FPS, so playback speed is correct when the
   card could not keep up.
6. **Minimum duration.** Delete segments below a configurable minimum
   duration (default 5 s) unless they are event-marked.
7. **Per-day folders** and file names that carry date/time, frame size,
   actual FPS and duration (R1 pattern). Public API still uses opaque IDs.
8. **Crash recovery (VisionCam improvement).** R1 only writes the header
   and index on close, so a power loss leaves an unplayable file. VisionCam
   Pro must:
   - keep segments short (default 60 s) so loss is bounded;
   - write a provisional header at segment start;
   - on boot, scan `*.tmp` segments, walk the `00dc` chunks sequentially
     (the layout is self-describing), stop at the first truncated chunk,
     truncate the file there, rebuild `idx1` and the header, and rename to
     the final name with a `recovered` flag in the index;
   - never delete an unrecovered segment automatically.
9. **Throughput budget.** Use the R1 table (Section 1A.4) as the initial
   expectation: about 20 fps at VGA/SVGA and about 5 fps at XGA and above
   with a good Class 2 or better card. Default capture FPS for event
   recording should stay well below those limits until measured.
10. **Writer isolation.** Only the storage manager's writer task touches
    the file; it uses a mutex only around header patching (R1 pattern),
    never around frame writes.

### Playback path decision (supersedes the earlier "custom browser AVI parser" default)

**Primary: server-side replay.** As in R1, the device reads the AVI from
SD in sector-multiple blocks, walks the `00dc` chunks and streams frames
to the browser as a timed `multipart/x-mixed-replace` MJPEG response at
the recorded FPS. The browser shows it in an `img`/canvas element, so no
JavaScript AVI parser is needed for the first release. Seeking is
implemented by restarting the replay at a frame offset taken from the
`idx1` index (a `start_frame` or `start_ms` parameter).

**Secondary (optional, later): client-side parser** that downloads the
file and parses frames, for offline review.

**Concurrency rule (diverges from R1).** R1 refuses playback while a
capture is running. VisionCam Pro must never let playback stop or stall
recording. Playback is allowed only if the SD read does not exceed a
configured share of measured card throughput and the playback task runs
at lower priority than the storage writer; otherwise return `503` with a
`PLAYBACK_BUSY` error code. Playback counts as one of the limited stream
slots.

### AVI writer requirements

-   Correct RIFF/AVI structure.
-   Correct stream headers and frame index.
-   JPEG frame chunks.
-   Correct timestamps/frame-rate metadata.
-   Segment finalization on duration/size threshold.
-   Recovery metadata for interrupted segments.
-   Validation with desktop media tools during development.
-   Never leave a single AVI open for hours.
-   Keep segments small enough for bounded recovery.

Suggested default segment duration: 60 seconds, configurable after
performance testing.

### Continuous recording policy

Continuous recording is disabled by default until the user explicitly
enables it. User can configure: - Enable. - Capture FPS. - Frame size. -
Segment duration. - Schedule. - Storage target. - Retention. - Maximum
storage percentage reserved for continuous footage.

### Event recording policy

On a motion or detection event: 1. Create event record. 2. Save event
snapshot. 3. Start event segment or mark the current segment. 4. Record
for configured post-event duration. 5. Apply cooldown. 6. Publish MQTT
event. 7. Update UI event list.

Pre-event recording requires a bounded circular buffer. Because full
JPEG frames consume substantial RAM, default pre-event duration should
be zero or very short until memory measurements validate a safe buffer.

## 4.7 Timelapse

Timelapse uses scheduled JPEG captures as source frames.

Configuration: - Enable. - Project name. - Capture interval. - Start/end
schedule. - Capture resolution. - JPEG quality. - Storage target. -
Playback FPS. - Retention.

Workflow: 1. Create project. 2. Capture JPEG frames on schedule. 3.
Store frames and index. 4. User selects project and playback FPS. 5.
Generate MJPEG AVI if resources permit. 6. Provide browser preview and
download. 7. Preserve source frames unless user deletes them.

Do not attempt high-resolution, long-duration video generation while
live streaming or continuous recording is under heavy load. Generation
must be a background job with progress, cancellation, and storage
checks.

### Reference-derived timelapse mode (from R1)

R1 writes time-lapse frames directly into an AVI as they are captured,
with the same header/index code and a separate index buffer, and stops
after a configured duration. This avoids a heavy "generate" step and the
risk of CPU/SD contention at generation time.

VisionCam Pro therefore supports two modes per project:

1. **Direct-to-AVI (default).** Each captured frame is appended to a
   time-lapse AVI; segments close at the configured duration or frame
   cap; playback FPS is stored in the header. No generation job is needed.
2. **Frames + generate (optional).** Keep individual JPEG source frames and
   build an AVI later as a background job, exactly as described above.
   Choose this only when the user wants to re-render at different FPS.

Rules taken from R1 and its weaknesses:

- Do not start a time-lapse until time is synchronised.
- R1 captures at the full recording FPS and picks one in N frames, which
  keeps the camera streaming continuously. VisionCam Pro should instead
  use the low-rate camera mode (`fb_count = 1` on demand) when no stream or
  recording is active and the interval is long, to save power and
  heat. Verify that switching modes is reliable before shipping.
- Allow time-lapse and continuous recording to coexist (R1 makes dashcam
  and time-lapse mutually exclusive) only if Phase 5 measurements show
  enough SD and CPU headroom; otherwise enforce mutual exclusion with a
  clear UI message.

## 4.8 Storage manager

microSD is primary storage. Optional network storage is an adapter, not
a dependency.

Storage functions: - Mount/unmount state. - Capacity, used and free
bytes. - Directory listing. - File metadata. - Delete one file. - Bulk
delete. - Download. - Retention. - Integrity scan. - Index rebuild. - SD
card error reporting. - Safe format action with explicit confirmation. -
Read-only degraded mode when filesystem errors are detected.

### Reference-derived storage practices (from R1)

- Organise recordings in **per-day folders** and keep file names
  self-describing, while exposing only opaque IDs through the API.
- Keep directory/file vectors in PSRAM (R1 forces small allocations into
  PSRAM) and paginate; never keep a full listing in internal RAM.
- Free-space policy: when free space drops below a configured minimum,
  delete the **oldest day folder** first (R1), optionally after upload.
  VisionCam Pro extends this with category quotas, a preview of
  candidates, never deleting open segments, and a final fallback of
  stopping recording with a critical warning, as R1 does.
- A simple WebDAV server for SD management exists in R1; it is a
  post-v1 convenience and must not bypass authentication or the storage
  manager.

### Directory layout

``` text
/sdcard/
├── system/
│   ├── device.json
│   ├── config-backup.json
│   └── index/
│       ├── media-index.jsonl
│       └── event-index.jsonl
├── snapshots/
│   └── YYYY/MM/DD/
│       ├── manual/
│       ├── motion/
│       └── detection/
├── recordings/
│   ├── continuous/YYYY/MM/DD/
│   ├── events/YYYY/MM/DD/
│   └── manual/YYYY/MM/DD/
├── timelapse/
│   └── {project_id}/
│       ├── frames/
│       ├── project.json
│       └── output/
├── thumbnails/
├── models/
│   ├── staging/
│   ├── active/
│   └── backup/
└── logs/
```

Do not store mutable primary application configuration only on SD. Use
NVS as the authoritative configuration store and write an optional SD
backup for recovery/export.

### Retention manager

-   Configurable retention days by media category.
-   Configurable maximum storage percentage.
-   Never delete active/open recording segments.
-   Never delete the only copy of a model currently in use.
-   Delete oldest eligible media first.
-   Use a two-phase delete where appropriate.
-   Log every automatic deletion.
-   Provide preview of cleanup candidates.
-   Do not run cleanup while the filesystem is busy or in an error
    state.

### Index design

Use append-only JSON Lines (JSONL) for media and event indexes. JSONL is
simpler to recover than repeatedly rewriting a large JSON array.

Media record example:

``` json
{
  "schema_version": 1,
  "media_id": "med-000001",
  "type": "snapshot",
  "category": "motion",
  "path": "/snapshots/2026/11/15/motion/med-000001.jpg",
  "mime": "image/jpeg",
  "size": 28431,
  "created_at": "2026-11-15T10:30:00Z",
  "event_id": "evt-000123",
  "duration_ms": 0,
  "status": "ready"
}
```

Event record example:

``` json
{
  "schema_version": 1,
  "event_id": "evt-000123",
  "type": "motion",
  "created_at": "2026-11-15T10:30:00Z",
  "ended_at": "2026-11-15T10:30:08Z",
  "snapshot_id": "med-000001",
  "recording_id": "rec-000009",
  "detections": [],
  "notification_status": "queued"
}
```

Requirements: - Validate every line independently. - Ignore malformed
trailing records during recovery. - Rebuild indexes by scanning
directories when necessary. - Use atomic temp-file replacement for
compacted indexes. - Do not load the entire media library into RAM. -
Paginate directory and API results. - Keep index entries small. - Treat
filesystem as source of truth; indexes are rebuildable caches.

## 4.9 Optional network storage

Create an interface:

``` cpp
class IStorageBackend {
public:
    virtual bool begin() = 0;
    virtual bool isAvailable() const = 0;
    virtual bool writeFile(const char* path, const uint8_t* data, size_t len) = 0;
    virtual bool removeFile(const char* path) = 0;
    virtual ~IStorageBackend() = default;
};
```

Initial required implementation: - `SdStorageBackend`

Optional implementations: - `HttpUploadStorageBackend` for uploading
completed files to a user-configured server. - `WebDavStorageBackend`
only if the chosen library and resource cost are acceptable. - NAS
integration through an external gateway is preferred over implementing a
complex filesystem client directly on the ESP32.

Network storage must use queued uploads of completed files. Do not
stream every frame synchronously to a remote NAS. Local SD storage
remains authoritative unless the user explicitly configures another
policy.

Configuration: - Enable/disable. - Backend type. - Server URL. -
Credentials/token. - Destination path. - Upload policy. - Retry count. -
Retry interval. - TLS verification. - Upload queue limit.

Secrets must be masked in the UI and stored securely as far as the
platform allows.

**Reference finding (R1 upload).** R1 uploads completed AVI files (and
whole day folders) to FTP/HTTPS servers, optionally deleting after
upload, and triggers it automatically when a recording closes. VisionCam
Pro keeps that "upload completed files, never frames" rule. Implement
`HttpUploadStorageBackend` first; FTP is deferred. TLS for HTTPS upload
needs substantial free heap (R1 uses about 64 KB as a threshold), so the
upload worker must check free heap and largest free block before starting
a TLS session and back off otherwise.

## 4.10 MQTT integration

Use MQTT 3.1.1 initially unless a tested client/library requires
otherwise.

Configuration: - Enable. - Broker host. - Port. - Username. -
Password. - Client ID. - Base topic. - QoS. - Keepalive. - TLS enable
and certificate settings. - Home Assistant discovery enable. - Discovery
prefix. - Availability topic. - Publish interval. - Event categories.

Default base topic:

`visioncam/{device_id}`

Topics:

``` text
visioncam/{device_id}/availability
visioncam/{device_id}/state
visioncam/{device_id}/status
visioncam/{device_id}/motion/detected
visioncam/{device_id}/motion/event
visioncam/{device_id}/detection/detected
visioncam/{device_id}/detection/event
visioncam/{device_id}/recording/status
visioncam/{device_id}/recording/event
visioncam/{device_id}/snapshot/created
visioncam/{device_id}/storage/used
visioncam/{device_id}/storage/free
visioncam/{device_id}/storage/percentage
visioncam/{device_id}/wifi/rssi
visioncam/{device_id}/ai/status
```

Example event:

``` json
{
  "schema_version": 1,
  "device_id": "vcam-a1b2c3",
  "event_id": "evt-000123",
  "event_type": "object_detected",
  "source": "external_ai",
  "label": "person",
  "confidence": 0.91,
  "timestamp": "2026-11-15T10:30:00Z",
  "snapshot_path": "/snapshots/2026/11/15/detection/evt-000123.jpg"
}
```

MQTT messages must not include credentials or large binary images. A
snapshot path is a local device path and is not automatically accessible
to Home Assistant. If remote access to snapshots is desired, use a
separately authenticated URL or network storage integration.

MQTT reliability: - Non-blocking publish queue. - Bounded queue size. -
QoS 0 for high-frequency telemetry by default. - QoS 1 for important
events where supported. - Reconnect with exponential backoff and
jitter. - Last Will retained offline state. - Publish retained online
state after connection. - Do not block camera or storage tasks waiting
for broker. - Drop/coalesce low-priority telemetry if queue is full. -
Preserve event records locally even if MQTT is unavailable.

### Reference-derived MQTT notes (from R1)

Adopted from R1:

- Use the ESP-IDF `esp-mqtt` client (3.1.1).
- Publish a **retained `online`** after connecting and register a
  **retained Last Will `offline`** (R1 uses QoS 2 for the Will; this
  specification requires at least QoS 1).
- Subscribe to a command topic and to Home Assistant's
  `homeassistant/status` birth topic.
- Check broker reachability before connecting (R1 starts after a ping
  succeeds) and keep the MQTT worker separate from camera tasks.

Deliberate divergences:

- R1 uses `homeassistant/sensor/{hostname}/...` topics and hostnames as
  identifiers. VisionCam Pro uses `visioncam/{device_id}/...` with a
  stable `device_id`, so entities survive renaming.
- R1 publishes a **camera image over MQTT** on motion for Home
  Assistant's MQTT camera. This specification keeps the rule that MQTT
  payloads never carry binary images (heap use, broker limits, privacy).
  Provide the snapshot through an authenticated URL instead. If the owner
  later wants MQTT images, add it as an opt-in setting, default off, with a
  hard size cap and QoS 0, and document the heap impact.
- R1 sends QoS 1 with retain 0 for state. VisionCam Pro keeps retained
  state only for availability and discovery.

## 4.11 Home Assistant MQTT Discovery

Use official Home Assistant MQTT Discovery format.

Default discovery prefix: `homeassistant`

Stable identifiers: - Device identifier: `visioncam_{device_id}` -
Entity unique IDs must remain stable across reboots and firmware
updates. - Device name: `VisionCam Pro {device_id}`

Recommended entities:

  Entity               HA platform
  -------------------- ---------------------------------------
  Camera online        binary_sensor
  Motion detected      binary_sensor
  Object detected      binary_sensor
  Person detected      binary_sensor, only if class exists
  Recording active     binary_sensor
  AI enabled           binary_sensor
  SD card mounted      binary_sensor
  Storage used         sensor
  Storage free         sensor
  Storage percentage   sensor
  Wi-Fi RSSI           sensor
  Inference time       sensor
  Start recording      button
  Stop recording       button
  Take snapshot        button
  Flash LED            switch, only if hardware mode is safe

Example discovery config:

``` json
{
  "name": "Motion",
  "unique_id": "visioncam_vcam_a1b2c3_motion",
  "state_topic": "visioncam/vcam-a1b2c3/motion/detected",
  "payload_on": "ON",
  "payload_off": "OFF",
  "availability_topic": "visioncam/vcam-a1b2c3/availability",
  "payload_available": "online",
  "payload_not_available": "offline",
  "device": {
    "identifiers": ["visioncam_vcam_a1b2c3"],
    "name": "VisionCam Pro vcam-a1b2c3",
    "manufacturer": "VisionCam Pro",
    "model": "AI-Thinker ESP32-CAM OV2640",
    "sw_version": "1.0.0"
  }
}
```

Discovery topic:

`homeassistant/binary_sensor/visioncam_{device_id}/motion/config`

Discovery requirements: - Publish retained discovery configuration. -
Publish state to matching state topics. - Use stable unique IDs. -
Support Home Assistant birth message on `homeassistant/status`. -
Republish discovery when HA reports `online`. - Publish offline via Last
Will. - Remove obsolete entities with empty retained discovery config
when feature/entity is intentionally removed. - Avoid publishing
discovery repeatedly on every loop. - Provide a "Republish discovery"
action in UI. - Make discovery optional and configurable.

Home Assistant control entities should use MQTT command topics and must
validate command payloads. All command actions must still obey device
authentication/security policy where an HTTP path is involved.

**Reference finding (R1).** R1's Home Assistant integration (contributed
module) sends discovery for an MQTT camera device and publishes motion
on/off for automations. VisionCam Pro keeps stable, per-entity discovery
(Section 4.11) and handles the birth message in the same way R1 does, but
does not create an image-based MQTT camera entity by default.

## 4.12 Time and clock

-   Use NTP when network is available.
-   Store timezone setting.
-   Use UTC internally for event timestamps.
-   Convert to configured local timezone for UI.
-   If time is unavailable after boot, mark timestamps as unsynchronized
    and reconcile after NTP sync.
-   Do not rely on compile-time date/time for media naming.
-   Provide manual time configuration fallback if required.

------------------------------------------------------------------------

**Reference finding (R1).** R1 sets time from NTP or from the connected
browser client, and refuses to start time-lapse until time is
synchronised. VisionCam Pro keeps NTP as the primary source and may accept
browser time only as an explicit, clearly marked fallback; any timestamp
produced before sync carries the `unsynchronized` flag.

------------------------------------------------------------------------

# 5. Web Application Specification

## 5.1 Frontend architecture

Use: - React. - TypeScript. - Vite. - CSS variables and a consistent
design system. - Lightweight charting only where necessary. - No
server-side runtime. - No external CDN dependencies at runtime. - Build
static assets and embed them into firmware filesystem/image or generated
C byte arrays.

The web application must remain usable when internet access is
unavailable.

Build output: - Minified JS/CSS. - Gzip-compressed assets where server
support is implemented. - Content-hashed filenames. - Embedded fallback
page for asset loading failure. - Asset manifest generated during build.

Do not embed huge, unoptimized development bundles. Measure final flash
usage and leave sufficient space for firmware, OTA strategy if used, and
model support. If the UI bundle makes firmware partitions impractical,
use a separate data partition or serve compressed assets from SD only as
an explicitly documented alternative. The default product should embed
the UI in firmware.

### 5.1.1 Reference-derived delivery decisions

- R1 stores its web pages on the SD card and downloads them from GitHub
  on first run. VisionCam Pro **rejects** this: it breaks local-first
  operation and makes the UI depend on the internet and on SD state. UI
  assets are embedded in firmware (Section 5.1).
- R1 uses a WebSocket for status and log streaming and Server-Sent Events
  for some updates, proving `httpd_ws` works on the original ESP32. VisionCam
  Pro may use a single WebSocket for status/log push with a strict client
  limit (suggested maximum 2), falling back to polling.
- R1's browser features are stated as fully tested only on Chrome. The UI
  must be tested on current Chrome, Firefox, Safari (iOS) and mobile
  Chrome.

## 5.2 UI visual direction

Design language: - Professional NVR / security operations dashboard. -
Dark theme as default. - Light theme optional. - Neutral charcoal
surfaces. - Clear status colors. - Compact but readable data density. -
Consistent spacing and typography. - Responsive desktop/tablet/mobile
layouts. - Avoid decorative gradients that reduce information clarity. -
Accessible contrast and keyboard focus. - Clear confirmation dialogs for
destructive operations.

Suggested palette:

  Token              Dark theme
  ------------------ ------------
  Background         `#101318`
  Sidebar            `#171C23`
  Surface            `#1D242D`
  Elevated surface   `#252E39`
  Border             `#303A46`
  Primary text       `#F1F5F9`
  Secondary text     `#A3AFBD`
  Accent             `#4F9CF9`
  Success            `#2FBF71`
  Warning            `#E6A23C`
  Danger             `#E05252`

## 5.3 Main application layout

Desktop: - Fixed left navigation sidebar. - Top header with page title,
device status, time, notifications, and user menu. - Main content
area. - Optional right-side details panel for selected media/event. -
Toast notifications. - Modal layer for confirmations and editing.

Mobile: - Collapsible navigation drawer. - Single-column layout. -
Stream player scales to viewport. - Touch-friendly controls. - Avoid
horizontal tables; use cards or responsive rows.

Navigation:

``` text
Overview
Live View
Recordings
Snapshots
Timelapse
AI Detection
Storage Manager
Integrations
  - MQTT
  - Home Assistant
  - Network Storage
Settings
  - General
  - Camera
  - Recording
  - Motion
  - AI Models
  - Network
  - Security
  - System
```

## 5.4 Page requirements

### Login

-   Product logo/name.
-   Username and password.
-   Show/hide password.
-   Login error without revealing which credential was wrong.
-   First-login password-change flow.
-   Session expiration handling.

### Overview

Cards: - Camera online/offline. - Live preview. - Recording state. -
Motion events today. - AI detections today. - SD card usage. - MQTT
status. - External AI status. - Recent events. - Recent snapshots.

### Live View

-   MJPEG player.
-   Stream state.
-   Snapshot button.
-   Start/stop recording.
-   Flash control if safe.
-   Camera resolution and quality controls.
-   Motion toggle.
-   AI toggle.
-   Full-screen view.
-   Connection/error indicator.

### Recordings

-   Calendar/date picker.
-   Filter by continuous/event/manual.
-   Recording list.
-   Timeline with event markers.
-   Custom MJPEG/AVI or JPEG-sequence player.
-   Playback speed.
-   Seek where index supports it.
-   Download.
-   Delete with confirmation.
-   Display recording integrity/status.

### Snapshots

-   Paginated thumbnail grid.
-   Filters: manual, motion, detection, timelapse.
-   Date range.
-   Full-screen viewer.
-   Previous/next.
-   Download/delete.
-   Metadata panel.
-   Event association.

### Timelapse

-   Create project.
-   Project list.
-   Capture interval.
-   Start/end schedule.
-   Resolution and quality.
-   Output FPS.
-   Progress indicator.
-   Cancel generation.
-   Preview.
-   Download output.
-   Manage source frames.

### AI Detection

-   Provider status: TFLite Micro / External / Disabled.
-   Active model and version.
-   Model compatibility result.
-   Upload model/manifest.
-   Activate/rollback.
-   Confidence threshold.
-   Detection interval.
-   External AI endpoint configuration.
-   Detection classes.
-   Recent events and snapshots.
-   Inference time and resource metrics.

### Storage Manager

-   Capacity bar.
-   Total/used/free.
-   Category breakdown if index supports it.
-   File browser.
-   Breadcrumbs.
-   Sort and filter.
-   Pagination.
-   Download/delete.
-   Retention policy.
-   SD health.
-   Index rebuild.
-   Safe format action with typed confirmation.

### Integrations

MQTT: - Broker host/port. - Credentials. - TLS configuration. - Base
topic. - Client ID. - Discovery prefix. - Connection test. - Save and
reconnect. - Publish test event. - Connection state.

Home Assistant: - Discovery enable. - Device/entity preview. - Discovery
republish. - Discovery status. - Birth message status. - Instructions
for broker setup.

Network Storage: - Enable. - Backend. - URL/path. - Authentication. -
Test connection. - Upload policy. - Queue status. - Retry/failure list.

### Settings

-   General/device name.
-   Wi-Fi.
-   Camera.
-   Recording.
-   Motion.
-   AI.
-   Storage/retention.
-   MQTT.
-   Network storage.
-   Security/user credentials.
-   Timezone/NTP.
-   System information.
-   Restart.
-   Firmware update only if a safe OTA design is implemented.

## 5.5 Frontend state management

Use a small predictable state solution: - React context or Zustand for
session/device state. - TanStack Query or a lightweight API cache for
REST resources if bundle size allows. - WebSocket or Server-Sent Events
only if implemented reliably on target; otherwise use polling with
sensible intervals. - Separate stream connection from API state. -
Cancel stale requests on navigation. - Paginate media and events. -
Never preload the entire media index. - Do not cache credentials in
localStorage. - Keep session authentication in HTTP-only cookies.

------------------------------------------------------------------------

# 6. HTTP API Contract

Base prefix: `/api/v1`

All JSON responses use:

``` json
{
  "ok": true,
  "data": {},
  "error": null,
  "request_id": "req-000001"
}
```

Error response:

``` json
{
  "ok": false,
  "data": null,
  "error": {
    "code": "SD_NOT_MOUNTED",
    "message": "Storage is unavailable"
  },
  "request_id": "req-000002"
}
```

Use appropriate HTTP status codes. Never return stack traces or secrets.

### 6.0 HTTP server limits (from R1/R2)

- Use `esp_http_server` with `lru_purge_enable = true`.
- Set `max_open_sockets` to allowed HTTP clients plus stream slots and
  document the total; R1 uses `HTTP_CLIENTS + MAX_STREAMS` with
  `MAX_STREAMS = 4`. Start smaller on the original ESP32.
- Server task stack is 4 KB in R1 on ESP32 (7 KB on S3). Measure the stack
  high-water mark with TLS disabled and with the largest handler active.
- Each long-running response (live stream, playback, large download)
  holds one stream slot; the slot count is a hard limit.
- The R2 JPEG capture and stream handlers are the reference handler
  shape: set the multipart content type, send boundary, part header,
  frame, and return the buffer on every exit path.

## 6.1 Authentication endpoints

  ------------------------------------------------------------------------------
  Method                  Path                           Description
  ----------------------- ------------------------------ -----------------------
  POST                    `/auth/login`                  Login

  POST                    `/auth/logout`                 Logout

  GET                     `/auth/session`                Current session

  POST                    `/auth/change-password`        Change
                                                         username/password

  POST                    `/auth/complete-first-login`   Complete forced
                                                         credential change
  ------------------------------------------------------------------------------

Login request:

``` json
{
  "username": "admin",
  "password": "admin"
}
```

The response sets an HTTP-only session cookie. First-login response
indicates `password_change_required: true`.

## 6.2 Device and status endpoints

  Method   Path                Description
  -------- ------------------- ----------------------------------
  GET      `/device`           Device identity and firmware
  GET      `/status`           Combined health status
  GET      `/health`           Basic health
  GET      `/metrics`          Resource and performance metrics
  POST     `/device/restart`   Restart device

## 6.3 Camera endpoints

  Method   Path                 Description
  -------- -------------------- ------------------
  GET      `/camera/status`     Camera state
  GET      `/camera/settings`   Read settings
  PUT      `/camera/settings`   Update settings
  POST     `/camera/capture`    Capture snapshot
  POST     `/camera/flash`      Set flash state
  GET      `/camera/stream`     MJPEG stream

Camera stream must be authenticated. Use a cookie-authenticated stream
route, not credentials in query strings.

## 6.4 Recording endpoints

  Method   Path                          Description
  -------- ----------------------------- --------------------------
  GET      `/recording/status`           Recording state
  GET      `/recording/config`           Recording configuration
  PUT      `/recording/config`           Update configuration
  POST     `/recording/start`            Start manual recording
  POST     `/recording/stop`             Stop manual recording
  GET      `/recordings`                 Paginated recording list
  GET      `/recordings/{id}`            Recording metadata
  GET      `/recordings/{id}/download`   Download recording
  GET      `/recordings/{id}/play`       Timed MJPEG replay (server-side)
  DELETE   `/recordings/{id}`            Delete recording

## 6.5 Snapshot endpoints

  Method   Path                         Description
  -------- ---------------------------- -----------------
  GET      `/snapshots`                 Paginated list
  GET      `/snapshots/{id}`            Metadata
  GET      `/snapshots/{id}/image`      Image bytes
  GET      `/snapshots/{id}/download`   Download image
  DELETE   `/snapshots/{id}`            Delete snapshot

## 6.6 Timelapse endpoints

  Method   Path                                  Description
  -------- ------------------------------------- ---------------------
  GET      `/timelapse/projects`                 List projects
  POST     `/timelapse/projects`                 Create project
  GET      `/timelapse/projects/{id}`            Project details
  PUT      `/timelapse/projects/{id}`            Update project
  POST     `/timelapse/projects/{id}/start`      Start capture
  POST     `/timelapse/projects/{id}/stop`       Stop capture
  POST     `/timelapse/projects/{id}/generate`   Generate output
  GET      `/timelapse/jobs/{id}`                Generation progress
  GET      `/timelapse/projects/{id}/preview`    Preview
  GET      `/timelapse/projects/{id}/download`   Download output
  DELETE   `/timelapse/projects/{id}`            Delete project

## 6.7 Storage endpoints

  Method   Path                             Description
  -------- -------------------------------- ---------------------------------
  GET      `/storage/status`                Capacity and mount state
  GET      `/storage/files`                 Paginated file listing
  GET      `/storage/files/{id}`            File metadata
  GET      `/storage/files/{id}/download`   Download
  DELETE   `/storage/files/{id}`            Delete
  POST     `/storage/rebuild-index`         Rebuild index
  GET      `/storage/retention`             Read retention policy
  PUT      `/storage/retention`             Update retention policy
  POST     `/storage/format`                Format with strong confirmation

Never expose arbitrary filesystem paths directly in API routes. Use
opaque media IDs resolved by the storage manager. Validate path
traversal and symlink-like path manipulation even if the filesystem does
not normally support symlinks.

## 6.8 Motion and AI endpoints

  Method   Path                         Description
  -------- ---------------------------- -----------------------------
  GET      `/motion/config`             Motion configuration
  PUT      `/motion/config`             Update motion configuration
  GET      `/motion/status`             Motion status
  GET      `/ai/status`                 AI status
  GET      `/ai/providers`              Provider list
  GET      `/ai/models`                 Model list
  POST     `/ai/models/upload`          Upload staged model
  POST     `/ai/models/{id}/validate`   Validate model
  POST     `/ai/models/{id}/activate`   Activate model
  POST     `/ai/models/deactivate`      Disable AI
  POST     `/ai/models/rollback`        Roll back model
  GET      `/ai/events`                 Detection event list
  GET      `/ai/config`                 AI configuration
  PUT      `/ai/config`                 Update AI configuration

Uploads must be size-limited and streamed to a staging file. Do not
buffer an entire model in RAM.

## 6.9 MQTT and integration endpoints

  ------------------------------------------------------------------------------------------
  Method                  Path                                       Description
  ----------------------- ------------------------------------------ -----------------------
  GET                     `/integrations/mqtt`                       MQTT config/status

  PUT                     `/integrations/mqtt`                       Save MQTT config

  POST                    `/integrations/mqtt/test`                  Test connection

  POST                    `/integrations/mqtt/reconnect`             Reconnect

  POST                    `/integrations/mqtt/test-publish`          Publish test

  POST                    `/integrations/home-assistant/discovery`   Republish discovery

  GET                     `/integrations/home-assistant/status`      Discovery status

  GET                     `/integrations/network-storage`            Network storage config

  PUT                     `/integrations/network-storage`            Save config

  POST                    `/integrations/network-storage/test`       Test backend

  GET                     `/integrations/network-storage/queue`      Upload queue status
  ------------------------------------------------------------------------------------------

## 6.10 API pagination and filtering

List endpoints should accept: - `limit` with a strict maximum. -
`cursor` or `offset`. - `from` and `to` timestamps. - `type`. -
`sort`. - `order`.

Return: - `items` - `next_cursor` - `has_more` - `total_estimate` where
feasible.

Do not calculate expensive total counts on every request.

------------------------------------------------------------------------

# 7. Configuration and Persistence

Use NVS (`nvs_flash`) as authoritative settings storage. Use versioned
configuration structures and migration functions.

Example configuration:

``` json
{
  "schema_version": 1,
  "device": {
    "device_id": "vcam-a1b2c3",
    "device_name": "VisionCam Pro",
    "timezone": "Asia/Colombo",
    "ntp_enabled": true
  },
  "camera": {
    "frame_size": "VGA",
    "jpeg_quality": 12,
    "brightness": 0,
    "contrast": 0
  },
  "recording": {
    "continuous_enabled": false,
    "event_enabled": true,
    "segment_seconds": 60,
    "capture_fps": 2,
    "post_event_seconds": 10
  },
  "motion": {
    "enabled": true,
    "sample_interval_ms": 500,
    "threshold": 24,
    "minimum_changed_ratio": 0.08,
    "cooldown_seconds": 15
  },
  "ai": {
    "enabled": false,
    "provider": "none",
    "inference_interval_ms": 5000,
    "confidence_threshold": 0.6
  },
  "mqtt": {
    "enabled": false,
    "host": "",
    "port": 1883,
    "base_topic": "visioncam/vcam-a1b2c3",
    "ha_discovery_enabled": true,
    "discovery_prefix": "homeassistant"
  },
  "storage": {
    "retention_days": 14,
    "max_used_percent": 90,
    "network_storage_enabled": false
  }
}
```

This JSON is a logical schema example. Do not store all settings as one
large JSON string in NVS if it risks size limits. Use namespaced keys or
a versioned serialized configuration with explicit size checks.

Secrets such as Wi-Fi password, MQTT password, external AI token, and
account credentials must be stored separately from ordinary settings and
never returned by GET endpoints. GET responses should return booleans
such as `password_configured: true`.

Configuration update process: 1. Parse request. 2. Validate all fields
and ranges. 3. Build candidate configuration. 4. Persist candidate
safely. 5. Apply runtime changes. 6. Roll back runtime state if
application fails. 7. Return sanitized effective configuration.

------------------------------------------------------------------------

**Reference finding (R1).** R1 keeps settings in a key/value file loaded
into a PSRAM vector and mirrored with Preferences, keeps passwords out of
that file, uses a version key to detect obsolete configuration files, and
needs "Save + reboot" for many changes. VisionCam Pro keeps NVS as the
single authoritative store, versions and migrates it, writes only a
secret-free backup to SD, and must apply as many changes as possible
without a reboot (camera and motion settings must be live).

------------------------------------------------------------------------

# 8. Firmware Task and Resource Architecture

## 8.1 Task model

Suggested tasks, priorities, and stack sizes are starting estimates
only. Measure on the actual board and adjust.

  Task                         Priority Responsibility
  ------------------------ ------------ ------------------------------------
  Camera/frame manager             High Frame acquisition and distribution
  Stream server                    High MJPEG clients
  Storage writer                   High Sequential media writes
  Event manager                  Medium Event orchestration
  Motion engine                  Medium Sampled-frame analysis
  AI inference               Low/Medium TFLM or external dispatch
  MQTT worker                Medium/Low Publish/reconnect
  Network storage worker            Low Upload completed files
  Retention/index worker            Low Cleanup and indexing
  System monitor                    Low Metrics and watchdog health

Do not copy these numeric priorities blindly. Validate starvation,
watchdog, and stack high-water marks.

### 8.1.1 Reference values from R1 (starting points only)

R1 on the original ESP32: capture task priority 6 (4 KB stack), stream and
HTTP tasks priority 5 (HTTP 4 KB, first stream task 5 KB), playback
priority 4 (2 KB), telemetry 3, Telegram/email 1, MQTT task 4 KB. R1 creates
several tasks with stacks in PSRAM (`xTaskCreateWithCaps`) to save internal
RAM; evaluate this for low-rate tasks only and verify it is safe for any
task that performs SD or flash operations before using it. The capture
loop uses `vTaskDelayUntil` so it catches up after a long cycle.

Keep VisionCam's own measured values in `docs/ARCHITECTURE.md` and treat
the numbers above as an initial guess, not a requirement.

## 8.2 Frame manager

The frame manager is the only module allowed to acquire frames directly
from the camera driver.

Responsibilities: - Acquire frame. - Assign frame ID and timestamp. -
Expose immutable frame views to consumers. - Track
consumers/ownership. - Release camera buffer only when all consumers are
finished. - Drop frames for slow consumers according to policy. - Avoid
holding camera buffers while waiting on SD writes or network calls.

Potential implementation: - One camera capture task. - Bounded queue of
frame descriptors. - Reference-counted frame leases. - Copy only when a
consumer needs longer retention than the camera buffer lifetime. -
Separate low-resolution processing copy for motion/AI if memory permits.

## 8.3 Event bus

Use typed internal events: - `MotionStarted` - `MotionEnded` -
`ObjectDetected` - `SnapshotCreated` - `RecordingStarted` -
`RecordingStopped` - `StorageWarning` - `StorageError` -
`MqttConnected` - `MqttDisconnected` - `AiProviderFailed` -
`NetworkStorageUploadFailed`

Event payloads must be small. Large image data is referenced by media
ID/path, never placed into the event queue.

## 8.4 Resource monitor

Expose: - Free heap. - Minimum free heap. - Largest free block. - PSRAM
total/free. - Task stack high-water marks. - Frame capture rate. -
Stream frame rate. - Dropped frame count. - SD write duration and
failures. - Recording queue depth. - AI inference duration. - MQTT queue
depth. - Reset reason. - Uptime.

Use these metrics to drive degraded modes: - Reduce inference rate. -
Reduce stream frame rate. - Disable timelapse generation. - Stop
optional network uploads. - Stop recording safely if storage is
unreliable.

Never silently corrupt files to preserve a feature.

------------------------------------------------------------------------

### 8.4.1 Reference-derived diagnostics

- Keep a small **RTC RAM ring log** (R1: about 7 KB) that survives soft
  resets so the reset cause and last events are available after a crash;
  optionally mirror the log over WebSocket and to SD (SD logging slows
  recording in R1, so it is opt-in).
- Use heap warning thresholds as a starting point: free heap below about
  32 KB or largest free block below about 16 KB (R1). A TLS session needs
  about 64 KB of free heap in R1's experience.
- Implement R1's **crash-loop detection**: count consecutive abnormal
  resets across boots and show a prominent warning that suggests checking
  the power supply first (R1's own README says this is usually the cause).

------------------------------------------------------------------------

# 9. Error Handling and Degraded Modes

  -----------------------------------------------------------------------
  Failure                             Required behavior
  ----------------------------------- -----------------------------------
  Wi-Fi unavailable                   Continue local camera/SD operation
                                      where possible; retry Wi-Fi

  MQTT unavailable                    Continue local operation; bounded
                                      queue; reconnect

  External AI unavailable             Disable remote inference
                                      temporarily; retain motion/local AI

  SD card missing                     Disable recording/snapshot
                                      persistence; show critical warning

  SD card full                        Run retention; if still full, stop
                                      recording safely

  SD write error                      Close segment, mark failed, stop
                                      affected recording

  TFLM model invalid                  Keep model disabled; preserve last
                                      known-good model

  AI task over budget                 Reduce inference frequency or
                                      disable AI

  Time not synchronized               Mark timestamps unsynchronized;
                                      correct future records after NTP

  Browser disconnected                Stop stream client cleanly

  Power loss                          Recover indexes and incomplete
                                      segments on reboot

  Network storage unavailable         Keep local files; queue bounded
                                      uploads

  Authentication failure              Rate-limit and log security event
                                      without secrets
  -----------------------------------------------------------------------

------------------------------------------------------------------------

### 9.1 Camera NULL-frame recovery policy (from R1)

When `esp_camera_fb_get()` returns `NULL`:

1. Never retry in a tight loop; yield for about 10 ms to satisfy the task
   watchdog.
2. Attempt recovery at most once every 2 seconds, at most 10 consecutive
   times, starting with a sensor reset, then PWDN power-cycle and camera
   re-init.
3. Reset counters when a valid frame arrives.
4. After the limit, raise a critical `CameraFailed` event, stop camera
   consumers cleanly, and expose the state in the UI and MQTT; do not spin.
5. Count dropped and invalid frames in metrics.

------------------------------------------------------------------------

# 10. Security Requirements

1.  Force default password replacement.
2.  Hash passwords with salt.
3.  Generate session tokens using cryptographic randomness.
4.  Use HTTP-only session cookies.
5.  Protect all media and API endpoints.
6.  Rate-limit login attempts.
7.  Validate all JSON and multipart uploads.
8.  Enforce maximum upload size.
9.  Prevent path traversal.
10. Never expose raw filesystem paths to clients.
11. Mask integration secrets.
12. Do not log secrets.
13. Use TLS certificate validation for external HTTPS endpoints.
14. Support MQTT TLS when memory and certificate storage allow.
15. Do not expose public unauthenticated stream URLs.
16. Use CSRF protection for cookie-authenticated state changes.
17. Validate Origin/Host for browser state-changing requests where
    applicable.
18. Require confirmation for format, bulk delete, and credential reset.
19. Do not allow arbitrary firmware/model binary execution.
20. Keep the device LAN-only by default.
21. Do not implement on-device TLS in the first release on the original
    ESP32. R1 reports HTTPS is only usable on ESP32-S3 and can still be
    unstable there; keep the device LAN-only or behind a VPN/reverse proxy.
22. Do not copy R1's authentication model (HTTP Basic over HTTP with a
    cached credential compared by `strcmp`). Use salted hashes, constant-time
    comparison, sessions and CSRF protection as specified.
23. Never expose R1-style open remote-access shortcuts (port forwarding,
    external heartbeat services) in the default configuration.

Threat model and limitations must be documented. A small ESP32 camera
should not be represented as equivalent to a hardened commercial NVR.

------------------------------------------------------------------------

# 11. Testing and Acceptance Criteria

## 11.1 Hardware tests

-   [ ] Board detected and flashes via ESP32-CAM-MB.
-   [ ] OV2640 initializes.
-   [ ] PSRAM detected and reported correctly.
-   [ ] SD card mounts in 1-bit mode.
-   [ ] Camera and SD operate together.
-   [ ] Flash LED behavior documented with SD mounted.
-   [ ] Stable power under Wi-Fi transmit and camera load.
-   [ ] Brownout behavior recorded.

## 11.2 Authentication tests

-   [ ] Default login works on first boot.
-   [ ] First login forces password change.
-   [ ] Old default credentials no longer work.
-   [ ] Password survives reboot.
-   [ ] Logout invalidates session.
-   [ ] Session expires.
-   [ ] Protected media URLs reject unauthenticated requests.
-   [ ] Login throttling works.
-   [ ] No secret appears in logs/API responses.

## 11.3 Camera and streaming tests

-   [ ] Live stream loads on desktop.
-   [ ] Live stream loads on mobile.
-   [ ] Stream disconnect cleans resources.
-   [ ] Snapshot is valid JPEG.
-   [ ] Camera settings apply and persist.
-   [ ] Maximum client limit is enforced.
-   [ ] Streaming does not starve storage writer.

## 11.4 Storage tests

-   [ ] SD mount/unmount state is correct.
-   [ ] Snapshot writes and downloads.
-   [ ] Directory pagination works.
-   [ ] Delete works and updates index.
-   [ ] Index rebuild recovers missing entries.
-   [ ] Corrupt trailing JSONL entry is ignored.
-   [ ] Full-card condition is handled safely.
-   [ ] Active recording is not deleted.
-   [ ] Power interruption recovery works.

## 11.5 Recording tests

-   [ ] Continuous recording starts/stops.
-   [ ] Event recording starts/stops.
-   [ ] Segment closes at configured duration.
-   [ ] AVI validates in desktop tools.
-   [ ] Frame timestamps and rate are reasonable.
-   [ ] Interrupted segment is marked/recovered.
-   [ ] Playback works through supported custom player.
-   [ ] Downloaded segment is playable in a compatible media player.

## 11.6 Timelapse tests

-   [ ] Scheduled capture occurs.
-   [ ] Source frames persist.
-   [ ] Project metadata survives reboot.
-   [ ] Generation progress is visible.
-   [ ] Cancellation works.
-   [ ] Output frame order is correct.
-   [ ] Output is downloadable.
-   [ ] Generation does not corrupt concurrent recording.

## 11.7 AI tests

-   [ ] No-AI mode works.
-   [ ] Invalid model is rejected.
-   [ ] Unsupported operator is reported.
-   [ ] Tensor arena allocation failure is handled.
-   [ ] Last-known-good model remains recoverable.
-   [ ] Inference rate is configurable.
-   [ ] AI load does not crash streaming.
-   [ ] External AI timeout/retry works.
-   [ ] External AI outage does not stop local recording.

## 11.8 MQTT/Home Assistant tests

-   [ ] Broker connection succeeds.
-   [ ] Bad credentials show useful error.
-   [ ] Reconnect works after broker restart.
-   [ ] Last Will publishes offline.
-   [ ] Online availability publishes after reconnect.
-   [ ] Motion event publishes.
-   [ ] Detection event publishes.
-   [ ] Discovery entities appear.
-   [ ] Entity IDs remain stable across reboot.
-   [ ] HA birth message triggers discovery republish.
-   [ ] Removed entities are cleaned up.
-   [ ] Broker outage does not block camera tasks.

## 11.9 Long-run acceptance

Before calling the first release stable: - Run at least a 24-hour mixed
workload test. - Include live streaming, event recording, snapshots,
motion detection, MQTT reconnects, and storage usage. - Track free heap,
minimum heap, largest free block, task stack high-water marks, dropped
frames, SD errors, and reset count. - Run repeated
reboot/power-interruption recovery tests. - Test the exact SD card
models intended for deployment.

------------------------------------------------------------------------

## 11.10 Reference parity tests

-   [ ] R1 baseline recorded on the owner's board and card (Section
    1A.7).
-   [ ] VisionCam recording FPS at VGA and QVGA within the agreed
    tolerance of the R1 baseline on the same card.
-   [ ] GPIO4 lamp stays off with SD mounted (R1 workaround verified).
-   [ ] Camera init with retry and PWDN power-cycle verified by forcing a
    first-attempt failure.
-   [ ] NULL-frame recovery is rate-limited and does not trigger the task
    watchdog.
-   [ ] Segment interrupted by power loss is recovered by the chunk
    scanner and plays back.
-   [ ] Header FPS equals measured FPS within 5 percent.
-   [ ] Playback never stops or visibly stalls an active recording, or
    returns `PLAYBACK_BUSY`.
-   [ ] Motion option is disabled above SXGA with an explanatory message.
-   [ ] Only one copy of `esp32-camera` is linked.

------------------------------------------------------------------------

# 12. Phased Implementation Plan

Implement in phases. Every phase must compile, flash, and have a
reproducible test procedure before the next phase begins.

## Phase 0 --- Repository and platform baseline

Deliverables: - PlatformIO project. - Pinned platform and library
versions. - AI-Thinker board profile. - Camera pin map. - SD 1-bit
test. - PSRAM detection. - Serial diagnostics. - Build instructions. -
Hardware validation notes.

Acceptance: - Firmware flashes using ESP32-CAM-MB. - Camera captures
JPEG. - SD card mounts. - No unstable power behavior during basic tests.

## Phase 1 --- Core camera and web UI

Deliverables: - Camera manager. - Frame manager. - Wi-Fi manager. - HTTP
server. - Embedded React UI. - Login and forced password change. - Live
MJPEG. - Snapshot endpoint. - Camera settings. - System status page.

Acceptance: - Authenticated browser access. - Live view on phone and
laptop. - No unauthenticated stream access. - UI assets served without
internet access.

## Phase 2 --- Storage and snapshots

Deliverables: - Storage abstraction. - SD backend. - Directory
structure. - Snapshot manager. - JSONL index. - Gallery. - Image
viewer. - Download/delete. - Storage status. - Index recovery.

Acceptance: - Snapshots survive reboot. - Gallery pagination works. -
Index can be rebuilt.

## Phase 3 --- Recording and playback

Deliverables: - AVI/MJPEG writer. - Continuous recording. - Manual
recording. - Event recording foundation. - Segment rotation. - Recording
index. - Recording browser. - Custom playback. - Download.

Acceptance: - AVI segments validate. - Segments recover safely after
interrupted shutdown. - Live view remains responsive during recording.

## Phase 4 --- Motion and event engine

Deliverables: - Motion detector. - Threshold and cooldown settings. -
Event bus. - Motion snapshots. - Event recording. - Event list and UI
indicators.

Acceptance: - Configurable motion event triggers snapshot and
recording. - No unbounded event queue. - False-trigger rate can be
tuned.

## Phase 5 --- Timelapse

Deliverables: - Timelapse scheduler. - Project manager. - Source-frame
index. - Background generator. - Custom preview. - AVI export. -
Download and delete.

Acceptance: - A short timelapse project can be captured and generated
without disrupting the camera.

## Phase 6 --- AI framework

Deliverables: - Inference provider interface. - No-AI provider. - TFLite
Micro provider. - Model manifest and validator. - Staging upload. -
Model activation/rollback. - Detection event schema. - Optional external
AI client. - AI dashboard.

Acceptance: - Invalid model is safely rejected. - At least one
deliberately selected and benchmarked model runs within measured
resource limits, or on-device AI remains clearly marked unsupported for
that model. - External AI failure does not stop core camera operation.

## Phase 7 --- MQTT and Home Assistant

Deliverables: - MQTT configuration UI. - MQTT manager. - Event
publisher. - Availability. - HA discovery. - HA command handlers. -
Reconnect behavior.

Acceptance: - Discovery entities appear in Home Assistant. -
Motion/detection events publish. - Broker outage does not interrupt
recording.

## Phase 8 --- Network storage

Deliverables: - Storage backend interface. - Completed-file upload
queue. - First optional network backend. - Retry and queue management. -
Integration UI.

Acceptance: - Files upload after segment closure. - Network failure
preserves local files. - Queue remains bounded.

## Phase 9 --- Hardening and release

Deliverables: - Resource optimization. - Long-run tests. - Security
review. - Power-loss recovery. - Configuration migration. - Release
documentation. - Installation guide. - Troubleshooting guide. -
Versioned firmware release.

Acceptance: - 24-hour mixed workload test. - No unexplained resets. - No
unbounded memory growth. - Media recovery verified. - All optional
integrations fail gracefully.

------------------------------------------------------------------------

## Phase amendments (Revision 2)

- **Phase 0:** add "build and flash R1 unmodified, capture the baseline
  table (Section 1A.7)"; add "decide clean-room vs AGPL option (Section
  1A.2)"; add "ADR for PlatformIO platform version, ESP-IDF version and
  `sdkconfig.defaults` baseline"; add `reference/` checkout script; add R2
  as a pinned IDF component.
- **Phase 1:** camera init follows Section 4.1.1; add NULL-frame
  recovery (Section 9.1); server limits per Section 6.0.
- **Phase 3:** AVI writer follows the reference-derived design in Section
  4.6, including the crash-recovery scanner and server-side playback;
  acceptance includes the R1 parity tests.
- **Phase 4:** motion follows Section 4.3.1.
- **Phase 5:** time-lapse defaults to direct-to-AVI; "frames + generate"
  is optional.
- **Phase 6:** external AI is the primary supported path; TFLite Micro is
  experimental on the original ESP32 and may be declared unsupported.
- **Phase 7:** MQTT uses `esp-mqtt`; no image payloads by default.
- **Phase 8:** HTTP upload backend first; FTP and WebDAV deferred.

------------------------------------------------------------------------

# 13. Repository Structure

Recommended repository:

``` text
visioncam-pro/
├── AGENTS.md
├── SKILL.md
├── README.md
├── CHANGELOG.md
├── platformio.ini
├── partitions.csv
├── docs/
│   ├── PRODUCT_SPEC.md
│   ├── ARCHITECTURE.md
│   ├── HARDWARE.md
│   ├── API.md
│   ├── MQTT_HOME_ASSISTANT.md
│   ├── STORAGE_AND_RECORDING.md
│   ├── AI_MODELS.md
│   ├── SECURITY.md
│   ├── TEST_PLAN.md
│   └── UI_UX_SPEC.md
├── firmware/
│   ├── include/
│   ├── src/
│   │   ├── app/
│   │   ├── core/
│   │   ├── camera/
│   │   ├── frame/
│   │   ├── network/
│   │   ├── web/
│   │   ├── auth/
│   │   ├── motion/
│   │   ├── ai/
│   │   ├── recording/
│   │   ├── snapshot/
│   │   ├── timelapse/
│   │   ├── storage/
│   │   ├── mqtt/
│   │   ├── integrations/
│   │   └── system/
│   └── test/
├── webui/
│   ├── package.json
│   ├── vite.config.ts
│   ├── tsconfig.json
│   ├── src/
│   │   ├── app/
│   │   ├── components/
│   │   ├── features/
│   │   ├── hooks/
│   │   ├── lib/
│   │   ├── pages/
│   │   ├── styles/
│   │   └── types/
│   └── public/
├── tools/
│   ├── embed_web_assets.py
│   ├── validate_model.py
│   └── validate_recording.py
├── models/
│   └── README.md
└── test/
    ├── api/
    ├── integration/
    └── fixtures/
```

Build files for the ESP-IDF/PlatformIO layout (adapt names to the final
layout and verify against the pinned platform):

``` text
platformio.ini            # framework = espidf, pinned platform, board, partitions
sdkconfig.defaults        # PSRAM, 80 MHz flash/PSRAM, camera Kconfig, log levels
partitions.csv            # app (OTA slots if used), NVS, data
firmware/src/CMakeLists.txt
firmware/src/idf_component.yml   # espressif/esp32-camera pinned, esp-mqtt etc.
components/               # in-tree modules with explicit interfaces
```

Additions for Revision 2:

``` text
visioncam-pro/
├── docs/
│   └── REFERENCE_ANALYSIS.md     # adopt/adapt/defer/reject log, baseline data, ADRs
└── reference/                    # NOT part of the build
    ├── README.md                 # license warning (R1 is AGPL-3.0)
    ├── fetch_references.sh       # clones R1 and R2 at pinned commits
    └── pins.txt                  # R1 abe84d89..., R2 b3f7fe80... (update deliberately)
```

`reference/` is excluded from the firmware build, from release archives,
and from code search used when authoring new code under the clean-room
option, except for deliberate study.

The precise directory arrangement may be adapted by the agent, but
module boundaries and ownership rules must remain.

------------------------------------------------------------------------

# 14. AGENTS.md

The following instructions are mandatory for any coding agent working in
this repository.

## Project mission

Build VisionCam Pro, a standalone local-first surveillance camera for
the AI-Thinker ESP32-CAM OV2640 using PlatformIO with the ESP-IDF framework (no Arduino
layer), microSD storage, and an embedded React web UI.

## Mandatory rules

1.  Read `SKILL.md` and relevant documents in `docs/` before making
    changes.
2.  Never assume the target is ESP32-S3. It is the original ESP32.
3.  Never assume arbitrary TFLite models fit or run.
4.  Never promise native H.264/MP4 recording on the ESP32-CAM.
5.  Keep all core functionality operational without MQTT, external AI,
    internet, or network storage.
6.  Use AI-Thinker pin mapping from the board profile only.
7.  Use the ESP-IDF SDMMC host in 1-bit mode as the initial supported SD
    configuration.
8.  Treat GPIO4/flash LED and SD interaction as a hardware constraint.
9.  Only the frame manager may acquire/release camera driver frame
    buffers.
10. Only the storage manager may perform filesystem operations.
11. Never block camera capture/stream tasks on MQTT, external AI, or
    network storage.
12. Every queue must have a bounded capacity and documented overflow
    behavior.
13. Never allocate unbounded memory based on user input or media size.
14. Do not load the entire media index into RAM.
15. All media APIs must use opaque media IDs, not arbitrary filesystem
    paths.
16. All management, media, snapshot, and stream endpoints require
    authentication.
17. Never store or log plaintext passwords, tokens, or integration
    secrets.
18. Do not store session tokens in localStorage or URL parameters.
19. Validate every configuration update before persistence.
20. Preserve last-known-good model and configuration during updates.
21. Use versioned schemas for configuration, media index, event index,
    and API payloads.
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
30. Prefer safe degradation over attempting to keep every feature active
    under memory pressure.
31. Before designing a subsystem, read Section 1A and the mapped R1/R2
    code; record adopt/adapt/defer/reject in `docs/REFERENCE_ANALYSIS.md`.
32. Do not copy or mechanically translate R1 (AGPL-3.0) code unless the
    owner has chosen the AGPL option in writing. Never vendor R1 into the
    build.
33. Treat `espressif/esp32-camera` as a pinned dependency; never fork it or
    link two copies.
34. Treat R1's performance numbers as unverified targets until the Phase 0
    baseline on the owner's board reproduces them.
35. Do not import R1 behaviours that conflict with this specification
    (Basic auth, UI on SD, images over MQTT, playback refusal during
    recording, header-only-at-close recording) without a recorded owner
    decision.
36. Do not claim a feature is "like R1" unless it was compared against the
    pinned R1 commit.
37. Build with PlatformIO `framework = espidf` only. Do not add the Arduino
    core as a component without a recorded owner decision. Port R1's
    Arduino API usage with the ESP-IDF equivalents in Section 1A.10.

## Required workflow

Before implementation (reference step): - Read Section 1A. - Flash and
measure the R1 baseline on the target board. - Confirm which
`esp32-camera` copy, PlatformIO platform version and ESP-IDF version
will be used.

Before implementation: - Inspect repository and current platform
versions. - Confirm board profile and camera pin mapping. - Confirm
partition layout and flash capacity. - Verify PSRAM detection. - Verify
SD 1-bit mounting. - Create a minimal build and hardware smoke test.

For each task: 1. State the intended module and interface changes. 2.
Identify memory, timing, and hardware implications. 3. Implement the
smallest vertical slice. 4. Compile firmware and web UI. 5. Run
available unit/static tests. 6. Test on hardware when hardware behavior
is involved. 7. Update docs and tests. 8. Report completed work and
remaining limitations honestly.

## Coding standards

Firmware: - C++ with clear ownership and RAII where practical. - Avoid
exceptions and dynamic allocation in hot paths. - Use fixed-size or
bounded containers for real-time queues. - Check all return codes. - Use
explicit timeouts. - Keep ISR code minimal. - Use task watchdog
correctly. - Avoid long critical sections. - Avoid logging secrets or
high-frequency frame data. - Use compile-time board configuration.

Frontend: - React + TypeScript. - Functional components. - Typed API
client. - Centralized error handling. - Accessible controls. -
Responsive layout. - No external runtime CDN dependencies. - No
credentials in browser storage. - Keep bundle size appropriate for
embedded flash. - Lazy-load large pages where practical.

## Definition of done

A change is done only when: - Code compiles. - Relevant tests pass. -
Hardware-sensitive behavior is validated or explicitly marked
unverified. - Resource impact is measured or estimated. - Error and
degraded paths are implemented. - Documentation is updated. - No secrets
or unsafe defaults are introduced.

------------------------------------------------------------------------

# 15. SKILL.md

## Skill: VisionCam Pro Embedded Camera Development

### Purpose

This skill guides a coding agent through development of the VisionCam
Pro ESP32-CAM firmware and embedded React UI.

### Context to load

Before work, read: - `AGENTS.md` - `docs/ARCHITECTURE.md` -
`docs/HARDWARE.md` - `docs/API.md` - `docs/STORAGE_AND_RECORDING.md` -
`docs/AI_MODELS.md` - `docs/MQTT_HOME_ASSISTANT.md` -
`docs/SECURITY.md` - `docs/UI_UX_SPEC.md` -
`docs/REFERENCE_ANALYSIS.md`

Read only the relevant sections for a focused change, but always read
`AGENTS.md`.

### Hardware profile

-   Board: AI-Thinker ESP32-CAM.
-   MCU: original ESP32.
-   Camera: OV2640.
-   Programmer: ESP32-CAM-MB.
-   SD: onboard microSD via ESP-IDF SDMMC host, 1-bit.
-   Build: PlatformIO with `framework = espidf`.
-   Wi-Fi: 2.4 GHz.
-   Flash LED: GPIO4, shared-resource considerations with SD.

Never substitute ESP32-S3 assumptions.

### Development procedure

1.  Identify the subsystem being changed.
2.  Read its interface and current tests.
3.  Identify all producers/consumers of shared resources.
4.  Check stack, heap, PSRAM, flash, and timing impact.
5.  Define bounded queues and timeouts.
6.  Implement behind a stable interface.
7.  Add error and degraded-mode handling.
8.  Add tests.
9.  Build and run tests.
10. Update the relevant documentation.

### Camera/frame handling skill

-   Use the official camera driver.
-   Centralize frame acquisition.
-   Never let multiple modules independently manage camera buffers.
-   Release every frame exactly once.
-   Do not retain a camera frame across blocking I/O.
-   Use downsampled/copy buffers for processing if needed.
-   Limit concurrent streams.
-   Measure dropped frames and capture rate.

### Storage skill

-   All filesystem operations go through StorageManager.
-   Use opaque IDs in APIs.
-   Use append-only JSONL indexes.
-   Make indexes rebuildable.
-   Write media to temporary/in-progress names and finalize after close.
-   Flush and close segments periodically.
-   Handle full card and write failure.
-   Never delete open files.
-   Do not format automatically.

### Recording skill

-   Use JPEG/MJPEG recording.
-   Do not implement fake MP4/H.264.
-   Segment files.
-   Validate AVI output using external tools in development.
-   Keep playback compatible with the implemented custom player.
-   Record format/version metadata.
-   Recover interrupted segments safely.

### AI skill

-   Use provider abstraction.
-   Treat TFLite Micro as optional and model-specific.
-   Validate operators and tensor arena.
-   Stage model uploads.
-   Keep last-known-good model.
-   Do not block streaming/storage on inference.
-   External inference uses timeout and bounded queues.
-   Normalize detections into common schema.

### MQTT/Home Assistant skill

-   Keep MQTT in its own worker.
-   Never block camera tasks on broker.
-   Use stable device/entity IDs.
-   Implement Last Will and retained discovery.
-   Handle HA birth message.
-   Preserve local event history when disconnected.
-   Never publish secrets or binary images.

### Frontend skill

-   Build React static assets using Vite.
-   Embed assets in firmware.
-   No runtime dependency on external internet.
-   Use typed API clients.
-   Use HTTP-only cookie sessions.
-   Paginate media.
-   Keep stream separate from query cache.
-   Provide loading, empty, error, and offline states.
-   Ensure mobile layouts work.

### Reference-first skill

1.  Look up the subsystem in the reference map (Section 1A.8).
2.  Read the R1 implementation and the R2 API for it. Write down, in your
    own words, what it does, its limits and its failure modes.
3.  Decide adopt, adapt, defer or reject; check the license policy
    (Section 1A.2) before writing code.
4.  Implement from the specification and the written notes, not from
    R1's text.
5.  Compare behaviour and numbers with the R1 baseline on hardware.
6.  Record the decision, the evidence and any divergence in
    `docs/REFERENCE_ANALYSIS.md`.
7.  If R1 and this specification disagree, the specification wins; flag
    the difference to the owner.

### Troubleshooting sequence

When the device crashes or becomes unstable: 1. Check reset reason and
serial logs. 2. Check free heap, minimum heap, largest block, and PSRAM.
3. Check task stack high-water marks. 4. Disable AI. 5. Disable external
network storage. 6. Disable continuous recording. 7. Test camera alone.
8. Test SD alone. 9. Test camera + SD. 10. Re-enable one subsystem at a
time.

Never "fix" instability by increasing arbitrary delays without
identifying the resource or synchronization issue.

------------------------------------------------------------------------

# 16. UI/UX Detailed Design Specification

## 16.1 Design principles

-   Professional surveillance/NVR product.
-   Information hierarchy before decoration.
-   Dark theme default, light theme optional.
-   Strong status visibility.
-   Large live video.
-   Clear recording state.
-   Minimal navigation depth.
-   Consistent destructive-action confirmations.
-   Responsive desktop and mobile behavior.
-   Accessible contrast, focus, and labels.

## 16.2 Desktop layout

``` text
+------------------------------------------------------------------+
| VISIONCAM PRO     Device: Online    MQTT: Connected   Admin  [v] |
+------------------+-----------------------------------------------+
|                  |                                               |
| Overview         | Page title                       [Actions]   |
| Live View        |                                               |
| Recordings       | Main content                                  |
| Snapshots        |                                               |
| Timelapse        |                                               |
| AI Detection     |                                               |
| Storage          |                                               |
| Integrations     |                                               |
| Settings         |                                               |
|                  |                                               |
|                  |                                               |
+------------------+-----------------------------------------------+
| Firmware version | SD: Mounted     Wi-Fi: Connected              |
+------------------------------------------------------------------+
```

Recommended proportions: - Sidebar: 220--250 px. - Header: 60--68 px. -
Main content padding: 20--28 px. - Cards: 12--16 px radius. - Consistent
8 px spacing scale.

## 16.3 Overview wireframe

``` text
+------------------------------------------------------------------+
| Overview                                  [Capture] [Record]      |
+------------------------------------------------------------------+
| [Camera Online] [Recording] [Motion Today] [AI Events]            |
+---------------------------------------+--------------------------+
|                                       | Device Health            |
|          LIVE CAMERA PREVIEW          | Wi-Fi RSSI               |
|                                       | SD Card Usage            |
|                                       | MQTT Status              |
|                                       | AI Provider              |
+---------------------------------------+--------------------------+
| Recent Events                         | Recent Snapshots         |
| 10:30 Person detected                 | [img] [img] [img]        |
| 10:25 Motion detected                 |                          |
+---------------------------------------+--------------------------+
```

## 16.4 Live View wireframe

``` text
+------------------------------------------------------------------+
| Live View                                      [Fullscreen]      |
+------------------------------------------------+-----------------+
|                                                | Camera Controls |
|                                                | Resolution      |
|                 LIVE VIDEO                     | Quality         |
|                                                | Brightness      |
|                                                | Contrast        |
|                                                |                 |
|                                                | [Snapshot]      |
|                                                | [Record]        |
|                                                | [Flash]         |
+------------------------------------------------+-----------------+
| [Motion Detection ON] [AI Detection OFF] [Stream FPS]             |
+------------------------------------------------------------------+
```

## 16.5 Recording playback wireframe

``` text
+------------------------------------------------------------------+
| Recordings                                      [Date selector]   |
+--------------------+---------------------------------------------+
| Calendar           | Recording Player                            |
|                    |                                             |
|  Event filters     |                                             |
|  Continuous        |              VIDEO / FRAME PLAYER           |
|  Motion            |                                             |
|  AI Detection      |                                             |
|                    |                                             |
+--------------------+---------------------------------------------+
| Timeline: 00:00 ---- event ---- recording ---- event ---- 23:59  |
+------------------------------------------------------------------+
| [Play] [Pause] [Speed] [Download] [Delete]                        |
+------------------------------------------------------------------+
```

## 16.6 Responsive behavior

Desktop: - Full sidebar. - Multi-column cards. - Side panels. - Full
timeline.

Tablet: - Collapsible sidebar. - Two-column cards. - Player controls
below video.

Mobile: - Drawer navigation. - One-column cards. - Video width 100%. -
Touch-friendly controls. - Media grid 2 columns where space permits. -
Timeline scrolls horizontally. - Avoid hover-only actions.

## 16.7 UI states

Every page must include: - Initial loading. - Empty state. - Error
state. - Offline/degraded state. - Success feedback. -
Permission/authentication failure. - Confirmation for destructive
action.

Never show a green "Connected" indicator based only on a saved
configuration. It must reflect an actual successful connection.

------------------------------------------------------------------------

# 17. Release and Documentation Requirements

Release package must include: - Firmware binary. - Firmware version and
build metadata. - Web UI version. - Supported board and SD card notes. -
Flashing instructions. - First-login instructions. - Wi-Fi setup
instructions. - SD card formatting recommendation. - MQTT setup guide. -
Home Assistant discovery guide. - AI model compatibility guide. -
External AI server API guide. - Recording format and playback guide. -
Backup/restore guide. - Troubleshooting. - Security limitations. - Known
issues.

Every release must report: - Firmware size. - Web asset size. - Free
flash/partition headroom. - Minimum free heap during representative
workload. - PSRAM use. - Stream frame rate. - Recording frame rate. - AI
inference duration for each supported model. - SD write performance. -
Long-run test duration and result.

------------------------------------------------------------------------

# 18. Initial Implementation Priorities

Do not attempt to implement all subsystems at once.

The first coding-agent task should be:

**Build Phase 0 and Phase 1 only.**

Specifically: 1. Create the PlatformIO project. 2. Confirm AI-Thinker
camera initialization. 3. Confirm PSRAM. 4. Mount SD card in 1-bit mode.
5. Implement Wi-Fi connection. 6. Implement secure single-user login and
forced password change. 7. Implement one authenticated MJPEG stream. 8.
Implement snapshot capture. 9. Build the first React dashboard and embed
it. 10. Add resource metrics and a hardware smoke-test guide.

Only after this stable foundation should the agent implement storage
indexing, recording, playback, motion, AI, MQTT, and Home Assistant.

## Reference-first note for the first coding task

Insert a step before step 1: **flash R1 unmodified on the target board
and record the baseline**, then proceed. Use the Section 1A.8 map while
implementing the camera init, snapshot and stream handlers.

## First release definition

The first usable release should provide: - Local Wi-Fi camera. -
Authenticated dashboard. - Live stream. - Snapshots saved to SD. - Image
gallery. - Basic motion detection. - MQTT motion notifications. - Home
Assistant motion binary sensor.

Continuous recording, event recording, timelapse, TFLite Micro, external
AI, network storage, and advanced playback should follow as separately
tested milestones.

------------------------------------------------------------------------

# 19. Official Technical References

Use these as the primary references and verify API details against the
exact pinned versions selected for the repository.

-   Espressif `esp32-camera`: https://github.com/espressif/esp32-camera
-   Reference R1 (application reference, AGPL-3.0):
    https://github.com/s60sc/ESP32-CAM_MJPEG2SD. Analysed at commit
    `abe84d89fc74367c21b644cb5101c9b89fd4bfb7` (APP_VER 10.10.1).
-   Reference R2 (camera driver, Apache-2.0):
    https://github.com/espressif/esp32-camera, Component Registry
    https://components.espressif.com/components/espressif/esp32-camera.
    Analysed at commit `b3f7fe802f41f17fd7a104d517c53f1f62c8414d`
    (registry version 2.1.9).
-   ESP-IDF Programming Guide (select the pinned version):
    https://docs.espressif.com/projects/esp-idf/en/stable/esp32/
-   ESP-IDF SD/MMC host and FATFS API (under Storage, same guide).
-   PlatformIO ESP-IDF framework:
    https://docs.platformio.org/en/latest/frameworks/espidf.html
-   ESP-IDF Component Manager (`idf_component.yml`):
    https://docs.espressif.com/projects/idf-component-manager/
-   TensorFlow Lite Micro: https://github.com/tensorflow/tflite-micro
-   TensorFlow Lite Micro documentation:
    https://www.tensorflow.org/lite/microcontrollers
-   Home Assistant MQTT integration:
    https://www.home-assistant.io/integrations/mqtt/
-   Home Assistant MQTT Discovery:
    https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery
-   MQTT specification:
    https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html
-   React documentation: https://react.dev/
-   Vite documentation: https://vite.dev/

------------------------------------------------------------------------

# 20. Final Engineering Guidance

VisionCam Pro should be treated as a constrained embedded NVR, not as a
desktop NVR compressed into an ESP32.

The most important architectural decisions are:

1.  Centralized camera frame ownership.
2.  A single storage manager.
3.  Bounded asynchronous queues.
4.  Segmented JPEG/MJPEG recording.
5.  A custom browser playback path.
6.  Local-first event handling.
7.  Optional, isolated AI providers.
8.  MQTT and Home Assistant as integrations, never core dependencies.
9.  Embedded React assets with no internet runtime dependency.
10. Measured performance on the exact AI-Thinker hardware.
11. Reuse proven designs from the reference projects instead of
    reinventing them, and document every deliberate divergence.

**The key product promise:** VisionCam Pro continues to capture, record,
and manage its local media even when optional cloud, AI, MQTT, or
network storage services are unavailable.

Build incrementally, measure resource usage, validate on the physical
board, and never claim a feature is supported until its hardware
acceptance test passes.
