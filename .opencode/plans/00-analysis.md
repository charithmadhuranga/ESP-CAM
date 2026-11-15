# VisionCam Pro — Project Analysis

Analysed inputs: `SPEC.md` (rev 2, 3,230 lines), `DESIGN.md` (209 lines),
`mockup-ui/` (17 desktop artboards), `reference/` (R1 + R2 checkouts).

---

## 1. Repository inventory (state at analysis time)

| Path | What it is | Role in the build |
|---|---|---|
| `SPEC.md` | Engineering specification, revision 2, sections 1–20 | **Normative.** Requirements, API contract, phases, test criteria |
| `DESIGN.md` | Web UI design reference derived from mockups + SPEC §5/§16 | **Normative for UI.** Wins where mockup and SPEC differ |
| `mockup-ui/*.png` | 17 artboards, fixed 1280x760, desktop only | Visual source of truth for layout and components |
| `reference/ESP32-CAM_MJPEG2SD-master/` | R1, `APP_VER 10.10.1`, commit `abe84d89…`, **AGPL-3.0** | Behavioural reference only (clean-room default) |
| `reference/esp32-camera-2.1.9/` | R2 camera driver, commit `b3f7fe80…`, **Apache-2.0** | Runtime dependency + API reference |
| `firmware/`, `webui/`, `docs/`, `platformio.ini`, `partitions.csv` | **Absent** | Everything is greenfield |
| Git repository | **Absent** | Phase 0 must `git init` and commit the spec baseline |

**Conclusion:** this is a documentation-complete, code-empty repository. The
first coding task is SPEC §18: build Phase 0 and Phase 1 only.

---

## 2. Product summary

VisionCam Pro is a standalone, local-first surveillance camera on the
AI-Thinker ESP32-CAM (original ESP32, OV2640, onboard microSD, ESP32-CAM-MB
programmer). It serves its own authenticated React+TypeScript web UI, an
authenticated MJPEG live stream, snapshot and AVI/MJPEG recording to SD,
motion detection, optional on-device TFLite Micro or external HTTP AI, MQTT
3.1.1 with Home Assistant discovery, and optional network upload.

**Key product promise:** camera, recording, snapshots, local playback and
administration keep working when Wi-Fi, MQTT, external AI, internet or
network storage are unavailable.

---

## 3. Hard constraints (never violate)

1. **Original ESP32**, not S3. No hardware H.264 → no MP4/H.264, no
   unrestricted multi-stream.
2. **PlatformIO `framework = espidf` only.** No Arduino core without a
   recorded owner decision (SPEC §1A.10, AGENTS rule 37).
3. **AI-Thinker pin map only**, from one board-profile header (SPEC §2.2).
4. **SD in 1-bit SDMMC mode**, `format_if_mount_failed = false` (SPEC §2.3).
5. **GPIO4** is shared between flash LED and SD — hardware constraint.
6. **Single frame owner:** only the frame manager calls
   `esp_camera_fb_get`/`esp_camera_fb_return`.
7. **Single filesystem owner:** only the storage manager touches files.
8. **Every queue bounded**, with documented overflow behaviour.
9. **Never block** camera/storage tasks on MQTT, external AI or network
   storage.
10. **All media APIs use opaque IDs**, never filesystem paths.
11. **Every management/media/stream endpoint authenticated**; HTTP-only
    cookie sessions; CSRF on state changes; no tokens in URLs or
    `localStorage`.
12. **No runtime CDN/font/image dependency.** UI embedded in firmware.
13. **R1 (AGPL-3.0) code is never copied or vendored** under the default
    clean-room option.
14. **Pin every dependency**; never `latest`.
15. **Never claim a test passed unless it was actually run** (rule 26).

---

## 4. Authority order when documents disagree

```
1. SPEC.md            (except where it explicitly supersedes itself)
2. DESIGN.md          (UI only; explicitly overrides mockup + SPEC §5/§16)
3. mockup-ui/*.png    (layout/component visuals; placeholder [values] are not data)
4. reference/         (behavioural evidence only, never code)
```

Known self-supersessions inside SPEC — read these before coding:

| Earlier statement | Superseded by | Effect |
|---|---|---|
| §4.1 "two frame buffers after stability testing" | §4.1.1 | `fb_count = 2`, PSRAM, `CAMERA_GRAB_LATEST`; up to 4 only after Phase 0 measurement |
| §4.1 init at user frame size | §4.1.1 rule 1 | Init once at **largest** frame size the PSRAM holds, then `set_framesize` at runtime |
| §4.6 "custom browser AVI parser" default | §4.6 playback decision | **Server-side timed MJPEG replay** is primary; client parser optional/later |
| §4.7 generic workflow "generate AVI" | §4.7 reference-derived mode | **Direct-to-AVI is default**; frames+generate is optional |
| §4.4 on-device TFLM as a provider | §4.4 reference finding + Phase 6 amendment | **External AI is the primary path**; TFLM experimental and may be declared unsupported |
| §12 Phase 5 "background generator" | Phase 5 amendment | Direct-to-AVI first; generator optional |
| §12 Phase 7 generic MQTT | Phase 7 amendment | `esp-mqtt`, no image payloads by default |
| §12 Phase 8 generic backend | Phase 8 amendment | HTTP upload first; FTP/WebDAV deferred |

---

## 5. `DESIGN.md` analysis

Design principles: professional NVR dashboard, information hierarchy before
decoration, dark theme default, strong status visibility, large live video,
one click from sidebar, confirmation on every destructive action, a status is
"Connected" only on a real connection, fully offline.

- **Tokens:** 12 dark-theme colour tokens, system font stack (no Inter from
  CDN), 5 type roles, spacing 4/8/12/16/24, radii 14/10/8/99/5, sidebar
  230px, header 60px, footer ~36px, 44px touch targets.
- **Shell:** Sidebar | Header (Device/MQTT/Admin pills) / Main (24px padding,
  16px gap) / Footer (firmware, SD, Wi-Fi). Toast + modal layers above all.
- **Navigation:** Overview, Live View, Recordings, Snapshots, Timelapse, AI
  Detection, Storage, Integrations, Settings (8 sub-sections).
- **Responsive:** desktop full / tablet collapsible sidebar 2-col / mobile
  drawer 1-col, 2-col media grid, horizontally scrolling timeline.
- **Components:** Card, StatCard, Row, Primary/Secondary/Danger button, Pill,
  Input+label, ProgressBar, VideoPlayer, Thumbnail, NavItem, KeyValueList,
  WarningCallout. A11y: real elements, `aria-label` on icon-only buttons,
  visible focus ring, 4.5:1 contrast. One embedded inline-SVG stroke icon set.
- **States every page must have (SPEC §16.7):** loading, empty, error+retry,
  offline/degraded banner, success toast, auth failure/session expiry,
  destructive confirmation.
- **Deliberate deviation recorded:** danger text `#F26B6B` (mockup) vs SPEC
  `#E05252` (fills/borders).

**`DESIGN.md` §8 open items (not yet designed):** mobile/tablet layouts, light
theme, all §6 states, snapshot full-screen viewer + metadata panel, recording
download/integrity detail, header notifications + user menu, first-login
password-change screen, Integrations sub-pages as separate views.

---

## 6. Mockup UI analysis — 17 artboards

| File | Screen | Notes for implementation |
|---|---|---|
| `Login.png` | Login | Centred 360px card, no sidebar; generic error; needs forced password change + session-expired states (not drawn) |
| `Main.png` | Overview | 4 stat cards, 2fr/1fr preview + device health, recent events / recent snapshots, Capture + Record actions |
| `Live.png` | Live View | Video + REC pill, 260px controls panel (resolution, quality, brightness, contrast, Snapshot/Record/Flash), status pill strip, Fullscreen |
| `Recordings.png` | Recordings | 240px sidebar (calendar + 4 filters), player, timeline with event markers, Play/Pause/1x/Download/Delete, Integrity OK badge |
| `Snapshots.png` | Snapshots | Filter pills, 5-column grid, pagination |
| `Timelapse.png` | Timelapse | Project list + 340px new-project form, progress, preview, download |
| `AI.png` | AI Detection | 3 cards (Provider / Detection / External AI server) over Recent detections + Metrics |
| `Storage.png` | Storage Manager | Capacity bar + category breakdown + health, breadcrumb file list with Download/Delete, retention policy, Rebuild index / Format card |
| `Integrations.png` | Integrations | 3 equal cards: MQTT, Home Assistant, Network Storage |
| `Settings.png` | Settings › General | 200px section list + form card; 8 sections |
| `SettingsCamera.png` | Settings › Camera | frame size, quality, FPS limit, flash, brightness/contrast/saturation/sharpness, AE/AWB, flip/mirror |
| `SettingsRecording.png` | Settings › Recording | mode, format, segment length, max FPS, pre/post event, enable toggles |
| `SettingsMotion.png` | Settings › Motion | sensitivity, threshold, zone editor, cooldown, min frames, triggers |
| `SettingsAIModels.png` | Settings › AI Models | installed models, status, activate/rollback, upload + validate, arena, timeout |
| `SettingsNetwork.png` | Settings › Network | SSID, password, IP mode, hostname, nearby networks, Scan, Save+reconnect |
| `SettingsSecurity.png` | Settings › Security | change password, session timeout, internet-exposure warning callout |
| `SettingsSystem.png` | Settings › System | firmware/UI versions, heap, PSRAM, uptime, backup/restore, restart, factory reset |

All artboards are 1280x760 desktop with `[placeholder]` values. The
implementation must be fluid and must supply mobile/tablet layouts itself
(`DESIGN.md` §3 responsive table).

### 6.1 Second pass — 11 screens decoded in detail

**Method and confidence.** The image `read` tool returned stale/cached
content for every PNG in that session, so all strings below were recovered by
direct pixel/glyph analysis (row/column ink projection + per-glyph bitmap
rendering). Colour values are sampled RGB. Artboards are **2560x1520 = 2x of
the 1280x760 CSS px**, so every coordinate below halves to CSS. Short labels
read from glyph bitmaps are high confidence; **spot-check any string against
the PNG before hard-coding it as UI copy.**

**Shared chrome — verified byte-identical across all 16 app screens**
(`Login.png` has none of it: no sidebar, no header pills, no footer —
confirmed by cropping Login's sidebar region x0-458 and finding **zero**
ink samples).

*Independently re-verified 2026-11-15 after the decode pass:*
- footer band (x500-1100, y1465-1495) → **one md5 across all 16** non-Login
  artboards;
- header pill band (x1950-2510, y45-80) → **one md5 across all 16**;
- sidebar band (x0-458, y150-1400) → 9 groups: the 8 Settings screens
  share one, and the other 8 differ *only* by which item is highlighted.

That is one shared component with an active-state prop, not 16 layouts.
(The original pass reported "10 screens"; the correct figure is 16.)

- Sidebar (CSS x0-229): `Overview`, `Live View`, `Recordings`, `Snapshots`,
  `Timelapse`, `AI Detection`, `Storage`, `Integrations`, `Settings`. Active
  item fill `rgb(37,46,57)`.
- Header pills (y49-75): `Device: Online` (green `Online`), `MQTT:
  Connected` (green `Connected`), `Admin`.
- Footer: hairline rule at y1444, then on every screen
  **`Firmware [version] | SD: Mounted | Wi-Fi: Connected`**. Note the mixed
  policy — `[version]` is bracketed-placeholder while `SD: Mounted` /
  `Wi-Fi: Connected` / `Device: Online` / `MQTT: Connected` render as real
  states.
- Palette sampled: page `rgb(16,19,24)`, card `rgb(29,36,45)`, row/active
  `rgb(37,46,57)`, hairline `rgb(48,58,70)`, primary text `rgb(241,245,249)`,
  secondary `rgb(163,175,189)`, blue `rgb(79,156,249)`, green `rgb(47,191,113)`,
  amber `rgb(230,162,60)`. ~13 CSS px card radius.

**Per-screen detail** (exact labels/values recovered):

- **Login** — `Username`, `Password`, bare-text `Show password` (no control
  drawn), full-width `Sign in`, red `Invalid username or password` shown by
  default. Inputs empty, no placeholders.
- **Snapshots** — title `Snapshots` + bordered `Date range` button. Filter
  pills `All` (active) / `Manual` / `Motion` / `Detection` / `Timelapse`.
  2x5 empty thumbnails labelled with times (row 1 `10:30 10:25 09:58 09:41
  09:12`, row 2 `08:55 08:30 08:02 07:44 07:15`). Pagination `Previous | 1 |
  2 | Next`. **No empty/loading/error state drawn for the grid** (G7).
- **Timelapse** — `Create project` (primary, top-right) + `Projects` card
  with 3 rows, all showing pixel-identical `Project name [sample]`:
  amber `Capturing` + ~60% blue progress bar; green `Ready` + `Preview` +
  `Download`; `Source frames` + `Manage`. `New project` card fields —
  `Capture interval` = `30 s`, `Schedule` = `Start / end time`,
  `Resolution and quality` = `SVGA, quality 12`, `Output FPS` = `15`.
  **No submit button inside that card** (G17).
- **Integrations** — 3 equal cards (only 3-column screen). `MQTT` + green
  `Connected`: `Broker host / port` = `[host]:1883`, `Base topic` =
  `visioncam/[id]`, `Client ID` = `[client id]`, `Test` + `Save and
  reconnect` (the only primary button). `Home Assistant` + green
  `Discovered`: `Discovery prefix` = `homeassistant`, `Birth message` =
  `Received`, `Entities` = `Motion, …`, `Republish discovery`.
  `Network storage` + gray `Disabled`: `Backend / URL` = `[backend]`,
  `Authentication` = `[user]`, `Upload queue` = `0 pending`,
  `Test connection`.
- **Settings › Camera** — `Resolution` = `SVGA 800x600`, `JPEG quality` =
  `12`, `Frame rate limit` = `[fps]`, `Flash LED` = `Safe mode`. 4 sliders
  in 2x2: `Brightness`, `Contrast`, `Saturation`, `Sharpness`. Text-only
  booleans: `Auto exposure` = `On`, `Auto white balance` = `On`, `Flip /
  mirror` = `Off`. `Save` + `Reset to default`.
- **Settings › Recording** — `Mode` = `Event-based`, `Format` = `MJPEG /
  AVI`, `Segment length` = `[minutes]`, `Max FPS` = `[fps]`, `Pre-event
  buffer` = `[seconds]`, `Post-event time` = `[seconds]`; `Continuous
  recording` = `Off`, `Record on motion` = `On`, `Record on AI detection` =
  `On`. `Save` + `Reset to default`.
- **Settings › Motion** — sliders `Sensitivity`, `Change threshold`; large
  `Detection zone editor` placeholder box; `Cooldown` = `[seconds]`,
  `Minimum frames` = `[n]`; `Start recording on motion` = `On`, `Publish
  MQTT motion event` = `On`. `Save` + `Reset to default`.
- **Settings › AI Models** — content card x940-2511. Model row 1: `[model]`
  `v[x]`, green `Active, validated`, `Rollback`. Row 2: `[model]` `v[y]`,
  amber `Incompatible operator`, `Activate`. Fields `Model file (.tflite)`
  = `Choose file`, `Manifest` = `Choose file`, `Tensor arena` = `[KB]`,
  `Inference timeout` = `[ms]`. Single primary `Upload and validate`;
  **no `Save`/`Reset` pair**.
- **Settings › Network** — `Wi-Fi network` = `[ssid]`, `Password` = dots,
  `IP mode` = `DHCP`, `Hostname` = `visioncam`. `Nearby networks` list:
  `[ssid]` = `-58 dBm`, `[ssid]` = `[dBm]`. `Scan` + `Save and reconnect`.
- **Settings › Security** — `Current password` / `New password` / `Confirm
  new password` = dots, `Session timeout` = `[minutes]`. Primary `Change
  password` only. Amber callout: **`Warning` / `Do not expose this device
  directly to the internet. Use a VPN or secure reverse proxy.`** — direct
  traceability to AGENTS rule 29.
- **Settings › System** — read-only rows `Firmware` `[version]`, `Web UI`
  `[version]`, `Min free heap` `[KB]`, `PSRAM free` `[KB]`, `Uptime`
  `[time]`. **Zero blue buttons** (verified by blue-pixel scan): `Back up
  config`, `Restore config`, `Restart`, `Factory reset` all secondary.
  Destructive gate `Type RESET to confirm factory reset` + empty input.

### 6.2 Findings that change implementation

| # | Finding | Consequence |
|---|---|---|
| G17 | **Timelapse "New project" card has no submit button** — 4 inputs and nothing else; creation is only offered by the top-right `Create project` above the whole list | Designer must decide: move the action into the card, or treat the top-right button as a modal trigger. Do not invent a button silently |
| G18 | **Action bar is inconsistent across settings sections** — Camera/Recording/Motion/Network = `Save` + `Reset to default`; AI Models = `Upload and validate` only; Security = `Change password` only; System = none | Decide per-section semantics vs drift before Phase 1 builds the shell. A user who mistypes a Security timeout currently has **no recovery path** |
| G19 | **No toggle widget exists in any mockup** — every boolean is `label … right-aligned value text` (`On` green / `Off` gray), no switch, checkbox or radio. Login's `Show password` is bare text | DESIGN.md must specify the actual control; the mockups cannot be conformed to literally |
| G20 | **Status is shown three different ways** — bordered badge (`Capturing`, `Ready`), colored inline text (`Active, validated`, `Connected`), right-aligned word (`On`/`Off`) | Pick one rule in DESIGN.md §4 before Phase 1, or the UI will look arbitrary |
| G21 | **Both AI-model file inputs read `Choose file`** (`.tflite` and `Manifest`) | Manifest input needs its own wording; confirm `Model file (.tflite)` label against SPEC §4.4 |
| G22 | **Destructive actions are not visually distinguished** — `Restart` and `Factory reset` are identical secondary buttons; only the `RESET` typing gate separates them | DESIGN.md requires confirmation on every destructive action; add danger styling (G9 colours) |
| G23 | **Snapshots grid has no empty/loading/error state**, only 2 pages of empty placeholders | Covered by G7's state classes, but note the media grid specifically needs them |
| G24 | **Chrome is genuinely shared** — independently re-verified: the footer band hashes to one md5 across **all 16** non-Login artboards, and Login has zero sidebar ink | Build one layout component; do not per-page it. But treat `Firmware [version]` as placeholder and the other four as live state |
| G25 | **No responsive artboards exist** — every screen is a single 1280x760 3-column shell (sidebar 229 + sub-nav ~223 + content) | Confirms G6: responsive behaviour is entirely DESIGN.md §3's job |
| G26 | **Real-looking defaults need SPEC cross-check** — `SVGA 800x600`, quality `12`, `Event-based`, `MJPEG / AVI`, `DHCP`, `visioncam`, `30 s`, `SVGA, quality 12`, `15`, `homeassistant`, `-58 dBm`, `0 pending` | Verify against SPEC §5/§16 defaults in Phase 1; bracketed tokens (`[version]`, `[fps]`, `[minutes]`, `[seconds]`, `[n]`, `[KB]`, `[ms]`, `[ssid]`, `[dBm]`, `[host]`, `[client id]`, `[backend]`, `[user]`, `[model]`, `v[x]`, `v[y]`, `[sample]`) are placeholders and must never ship as literal text |

**Reassuring:** placeholder discipline is consistent and **no credential,
token or secret appears anywhere** — passwords render as dots only, matching
AGENTS rules 17-18. The Security warning callout matches rule 29 exactly.

---

## 7. Reference projects analysis

### R1 — `s60sc/ESP32-CAM_MJPEG2SD` (AGPL-3.0)

Already runs on the owner's board → it is the **hardware baseline**
(SPEC §1A.7). About 17,000 lines under `src/`, Arduino framework, multi-feature.

Proven behaviours worth inheriting (adapt, never copy):
camera init at max frame size then runtime resize, retries with PWDN
power-cycle, sensor PID check, warm-up frames; `fb_count=4` + `GRAB_LATEST`
(VisionCam starts at 2); capture loop with `vTaskDelayUntil` and time-limited
NULL-frame recovery; AVI with reserved header patched at close, `00dc` chunks
4-byte padded, `idx1` in PSRAM, 8 KB sector-aligned write buffer, temp name
then rename, **measured** FPS in the header, 5 s minimum duration, per-day
folders; server-side timed MJPEG playback; motion via tiny grayscale decode
with dual sample rates and debug overlay; `SD_MMC` 1-bit + GPIO4-low
workaround; `esp-mqtt` with retained LWT; RTC RAM log ring; crash-loop
detector.

Known weaknesses **not** to copy: header/index only at clean close (no
crash-safety), playback refused during capture, motion disabled during
continuous recording, Basic auth over HTTP, UI on SD downloaded from GitHub,
"save + reboot" for many settings, images over MQTT, `homeassistant/sensor/
{hostname}` topics.

R1's own README states the original ESP32 **runs out of heap** for its full
feature set → VisionCam adopts R1's core (record, replay, motion, storage,
MQTT) and explicitly excludes audio, RTSP, telemetry, remote control,
photogrammetry, intercom, camera hub and on-device HTTPS.

### R2 — `espressif/esp32-camera` (Apache-2.0)

Adopted as a **pinned ESP-IDF component dependency**, never forked, never
duplicated. Facts that shape the firmware: PSRAM required above CIF in
JPEG; `fb_count=1` on-demand vs `>=2` continuous; `GRAB_LATEST` semantics;
`jpeg_quality` 0–63 lower-is-better; `jpeg_buffer_size` default
`width*height/5`; every `fb_get` must be `fb_return`ed; non-JPEG formats
limited to QVGA on ESP32; camera task pinned to core 0 with 4 KB stack;
unused sensors can be disabled via Kconfig; use `frame2jpg`/`fmt2rgb888`
helpers instead of writing converters.

**Build-system note:** VisionCam owns `sdkconfig`, so R2's Kconfig menu is
fully controllable — enable only OV2640, set camera task core, DMA buffer
size, JPEG buffer mode. Symbol names vary by IDF version → record the
baseline in `sdkconfig.defaults` + an ADR.

### Licence policy (SPEC §1A.2) — **owner decision required**

Default = **Option A clean-room**: VisionCam Pro stays under the owner's
licence; R1 is only a behavioural reference; no copying or mechanical
translation of R1 functions/tables/macros (explicitly the 310-byte AVI header
table and `frameSizeData`); R1 never linked into a release build.
Option B = adopt AGPL-3.0 and reuse R1 code with attribution.
R2 may be used under either option; keep its licence notice.

### 7.1 Spot-check of SPEC claims against the pinned source (done during analysis)

Every reference-derived claim below was grepped in the actual checkouts and
**confirmed**. This is evidence, not assumption — re-verify if the pins move.

| SPEC claim | Confirmed at |
|---|---|
| R1 `fb_count = 4` at init | `mjpeg2sd.cpp:20` `#define FB_CNT 4` |
| Max JPEG buffer `width*height/5` | `mjpeg2sd.cpp:1033` **and** R2 `cam_hal.c:619` + `Kconfig:255` |
| Retry init with PWDN power-cycle | `mjpeg2sd.cpp:1091-1094` |
| Sensor PID check, OV2640 required | `mjpeg2sd.cpp:1123` `case (OV2640_PID)` |
| Warm-up frames | `mjpeg2sd.cpp:1194` `warmupFrames = 3`, 150 ms apart |
| Capture task 4 KB stack, priority 6 | `appGlobals.h:104`, `:129` |
| `vTaskDelayUntil` catch-up loop | `mjpeg2sd.cpp:596`, `:740` |
| NULL-frame recovery ≤1/2 s, ≤10 attempts | `mjpeg2sd.cpp:394` `RECOVERY_INTERVAL_MS 2000`, `:395` `MAX_RECOVERY_ATTEMPTS 10` |
| Max 20,000 frames per segment | `mjpeg2sd.cpp:29` `maxFrames = 20000` |
| Motion not offered above SXGA | `motionDetect.cpp:729` `if (fsizePtr > FRAMESIZE_SXGA) return false;` |
| `lru_purge_enable = true` | `webServer.cpp:728`, `:751` |
| `max_open_sockets = HTTP_CLIENTS + MAX_STREAMS` | `webServer.cpp:731`, `:754`; `HTTP_CLIENTS 3` + `MAX_STREAMS 4` at `appGlobals.h:54-55` |
| 310-byte AVI header template | `avi.cpp:10` `310 bytes`, `AVI_HEADER_LEN` |
| `00dc` / `idx1` markers, 16-byte index entry | `avi.cpp:39`, `:41`, `:102` `#define IDX_ENTRY 16` |
| Index buffer `(maxFrames+2)*16` in PSRAM | `avi.cpp:115` `ps_malloc((maxFrames+2)*IDX_ENTRY)` |
| 8 KB sector-multiple write buffer | `appGlobals.h:76` `RAMSIZE (1024 * 8) // … multiple of SD card sector size` |
| Temp name then rename | `appGlobals.h:85-87` `AVITEMP "/current.avi"`, `TLTEMP "/current.tl"`; rename at `mjpeg2sd.cpp:322`, `:207` |
| Per-day folders | `utilsFS.cpp:233` `"/%Y%m%d"` |
| SD 1-bit default | `utilsFS.cpp:80` `SD_MMC.begin(..., use1bitMode, ...)` |
| RTC RAM ring log ≈7 KB | `globals.h:97` `RAM_LOG_LEN (512 * 13)` = 6,656 B |
| Crash-loop counter survives reset | `utilsLog.cpp:44` `RTC_NOINIT_ATTR uint32_t crashLoop` |
| Heap warning thresholds | `appGlobals.h:63-64` `WARN_HEAP (32*1024)`, `WARN_ALLOC (16*1024)` |
| TLS needs ≈64 KB free heap | `appGlobals.h:62` `TLS_HEAP (64 * 1024)` |
| MQTT via `esp-mqtt`, LWT QoS 2 retained | `mqtt.cpp:28-31`, `:260-264` |
| R1 uses hostnames as entity identifiers | `mqtt.cpp:246` `"%ssensor/%s/lwt"`, `:331` |
| R1 publishes an image over MQTT | `mqtt.cpp:129` `image_topic` → **spec rejects this by default** |
| R1 subscribes to HA birth topic | `mqtt.cpp:17` `#define HASIO_AVAILABILITY "homeassistant/status"` |
| `CAMERA_GRAB_LATEST` / `WHEN_EMPTY` semantics | R2 `esp_camera.h:90-91` |
| SD 1-bit is R1's **default**, not a fallback | `utilsFS.cpp:28` `static bool use1bitMode = true;` (set false only at `:72`) |
| Playback refused while capture runs | `mjpeg2sd.cpp:656` and `streamServer.cpp:51` `"Playback refused - capture in progress"` — **SPEC §1A.9 rejects this behaviour** (record divergence) |
| Motion runs inline with the capture loop | `mjpeg2sd.cpp:489` `checkMotion(fb, isCapturing)` → `motionDetect.cpp:718`; sample size from `frameData[].sampleRate` at `motionDetect.cpp:762` |
| Motion cadence: **5 checks/s idle, 1 check/2 s while recording** (SPEC §1A.9) | `mjpeg2sd.cpp:27-28` `moveStartChecks = 5`, `moveStopSecs = 2`; rate limiter `doMonitor()` at `mjpeg2sd.cpp:123-131`: `checkRate = capturing ? FPS*moveStopSecs : FPS/moveStartChecks`. Comment on `moveStopSecs` also says *"determines post motion time"* — matching SPEC's post-motion clause |
| **GPIO4 driven low after SD mount** (SPEC §2.3) | `utilsFS.cpp:81-84`, immediately after `SD_MMC.begin(...)`: `#if defined(CAMERA_MODEL_AI_THINKER)` → `pinMode(4, OUTPUT); digitalWrite(4, 0);` with comment *"sd_mmc library still initialises pin 4 in 1 line mode"* |
| SD pins: CLK 14, CMD 15, D0 2, D1 4, D2 12, D3 13 | Board-level fact supplied by the owner (2026-11-15); SPEC §2.2 tabulates only camera pins. Explains why 1-bit mode frees GPIO4 and GPIO12 (SPEC line 333) |

> **Method note (honesty record).** Two of the rows above were initially
> recorded as *unverified* in this file. Both were false negatives caused by
> my own search method, not by the source: the GPIO4 code was cut off by
> piping `grep` through `head -15`, and the motion cadence was searched for as
> a *timer* when the mechanism is a *frame counter* inside `doMonitor()`.
> Re-run both greps without truncation before ever repeating a negative
> finding (AGENTS rules 26, 36).

**New finding not called out in SPEC:** `appGlobals.h:58`
`JSON_BUFF_LEN (32 * 1024) // set big enough to hold all file names in a
folder` — R1 does load an entire folder listing into one 32 KB buffer.
VisionCam rule 14 ("do not load the entire media index into RAM") is a
**deliberate divergence**; keep it and record it in
`docs/REFERENCE_ANALYSIS.md`.

---

## 8. Gaps and conflicts found

| # | Gap / conflict | Where | Resolution in this plan |
|---|---|---|---|
| G1 | Licence option A vs B undecided | §1A.2 | Phase 0 owner gate; default A |
| G2 | R1 baseline not measured on owner's board | §1A.7 | Phase 0 hardware gate, blocks performance claims forever after |
| G3 | No git repo, no build files, no code | repo | Phase 0 |
| G4 | `reference/` has no `README.md`, `fetch_references.sh`, `pins.txt` | §13 | Phase 0 |
| G5 | SPEC §14 AGENTS.md and §15 SKILL.md exist only as spec text | §14/§15 | **This task** — written to `AGENTS.md` + `.opencode/skills/` |
| G6 | Mockups desktop-only; no mobile/tablet artboards | `DESIGN.md` §3 | Frontend phases must design responsive layouts from the DESIGN table |
| G7 | 8 UI state classes undrawn (loading/empty/error/offline/toast/auth/confirm) | `DESIGN.md` §6 | Build once in Phase 1 shell, reuse on every page |
| G8 | Snapshot viewer, recording integrity/download detail, notifications, user menu, first-login screen, Integrations sub-pages undrawn | `DESIGN.md` §8 | Scheduled into Phase 1 (login) and Phases 2/3/7 |
| G9 | Danger colour deviation `#F26B6B` vs `#E05252` | `DESIGN.md` §2.1 | Text `#F26B6B`, fills/borders `#E05252`; record in UI spec |
| G10 | `fb_count` 2 vs 4 | §4.1 vs §4.1.1 | Start 2, raise to 4 only after Phase 0 stability+heap data |
| G11 | Which `esp32-camera` copy is linked (registry 2.1.9 vs `library.json` 2.0.0) | §1A.1 | Pin exact version, assert single copy at build, record in ADR |
| G12 | ESP-IDF / PlatformIO platform version not chosen | §3.3.1 | Phase 0 ADR + `sdkconfig.defaults` baseline |
| G13 | Whether TFLM is ship-supported on original ESP32 | §4.4, Phase 6 | Phase 6 benchmark may legitimately end in "unsupported" |
| G14 | FPS parity tolerance vs R1 baseline | §1A.7 | **10 %, owner-confirmed 2026-11-15 (D3)** |
| G15 | Pre-event buffer RAM cost unknown | §4.6 | Default pre-event = 0 until measured |
| G16 | R1 loads a whole folder listing into one 32 KB buffer (`JSON_BUFF_LEN`) — conflicts with AGENTS rule 14 | `appGlobals.h:58` | Keep VisionCam's pagination; record as a deliberate divergence in `docs/REFERENCE_ANALYSIS.md` |

**G17–G26** are mockup-derived and live in §6.2 (second-pass decode of the
11 remaining artboards): missing Timelapse submit button, inconsistent
settings action bars, no toggle widget drawn, three different status
styles, duplicate `Choose file` labels, undistinguished destructive
buttons, no media-grid empty state, shared-chrome confirmation, no
responsive artboards, and real-looking defaults needing a SPEC §5/§16
cross-check. They are design decisions for `DESIGN.md`, not code gaps —
resolve them in Phase 1 before the shell is built.

---

## 9. Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| Heap exhaustion as features stack up | Crashes, brownouts | Degraded-mode ladder (SPEC §9), measure heap every phase, disable optional subsystems before core |
| SD write throughput below R1 baseline | Low recording FPS | 8 KB aligned writes, segment rotation, measure per card model, explain any shortfall |
| Power loss mid-recording corrupts AVI | Lost evidence | Short segments + provisional header + boot-time chunk scanner (SPEC §4.6) |
| Flash budget blown by React bundle | Cannot embed UI | Content-hash + gzip, measure against partition, lazy-load pages, no fonts/icon CDN |
| AGPL contamination via R1 | Licence obligation | Clean-room workflow enforced by `visioncam-reference-first` skill + `docs/REFERENCE_ANALYSIS.md` log |
| GPIO4 lamp/SD interaction | Hardware instability | Phase 0 test with ESP-IDF SDMMC driver, document result |
| TLS heap cost (~64 KB) | OOM on external AI/HTTPS upload | Heap gate before TLS sessions; LAN-only default; no on-device TLS in v1 |
| Two `esp32-camera` copies linked | Subtle driver bugs | Single pinned component, build-time assertion |

---

## 10. Traceability — spec → phase → skill

| Spec area | Phase | Skill |
|---|---|---|
| §1A reference analysis, §13 repo, §12 Phase 0 | 0 | `visioncam-reference-first`, `visioncam-build-flash` |
| §4.1 camera, §4.2 auth, §5 web app, §16 UI, §12 Phase 1 | 1 | `visioncam-dev`, `visioncam-webui`, `visioncam-hardware-validation` |
| §4.5 snapshots, §4.8 storage, §12 Phase 2 | 2 | `visioncam-dev`, `visioncam-api-contract` |
| §4.6 recording + playback, §12 Phase 3 | 3 | `visioncam-dev`, `visioncam-hardware-validation` |
| §4.3 motion, §8.3 event bus, §12 Phase 4 | 4 | `visioncam-dev` |
| §4.7 timelapse, §12 Phase 5 | 5 | `visioncam-dev` |
| §4.4 AI, §12 Phase 6 | 6 | `visioncam-dev`, `visioncam-api-contract` |
| §4.10 MQTT, §4.11 HA, §12 Phase 7 | 7 | `visioncam-dev`, `visioncam-api-contract` |
| §4.9 network storage, §12 Phase 8 | 8 | `visioncam-dev` |
| §9 degraded modes, §10 security, §11 tests, §17 release, §12 Phase 9 | 9 | `visioncam-hardware-validation`, `visioncam-dev` |
| §6 HTTP API, §7 configuration | all | `visioncam-api-contract` |
| §3 architecture, §8 tasks | all | `visioncam-dev` |
