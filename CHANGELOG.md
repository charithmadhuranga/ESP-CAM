# Changelog

All notable changes to VisionCam Pro are recorded here.
Format follows [Keep a Changelog](https://keepachangelog.com/); versions are
tagged once Phase 9 release criteria pass.

## [Unreleased]

### Fixed

- **Live View after reboot (2026-11-22).** Check the authenticated camera
  status before opening the MJPEG stream. Since sessions are intentionally
  RAM-only, a device reboot invalidates existing browser sessions; the UI
  now returns to sign-in instead of misreporting the protected stream as
  an unavailable camera.
- **Live View worker allocation (2026-11-22).** Reuse one stream worker
  created at server startup instead of allocating its task stack for every
  request. Allocate the bounded UXGA JPEG copy buffer from PSRAM so large
  frames do not depend on fragmented internal heap.
- **Camera startup after recovery testing (2026-11-22).** The destructive
  camera-recovery self-test is now opt-in and disabled in normal builds; the
  repeated shutdown/reinitialization sequence could fragment internal
  DMA-capable heap and prevent camera startup. The opt-in test now reports
  failure unless the camera is restored successfully.
- **Live View streaming (2026-11-22).** Corrected MJPEG multipart framing
  and moved the bounded stream to an ESP-IDF asynchronous request worker so
  it no longer blocks capture, flash, or other HTTP controls. The UI now
  reports a no-frames timeout instead of staying on “Connecting…” forever.
  Fixed frame-manager cross-core reference races by acquiring a queue
  reference before publishing a record and delaying pool reuse until the
  final camera-buffer release has completed.
  Added authenticated, CSRF-protected flash control; GPIO4 is driven low
  after SD 1-bit mount and when the stream closes. The final flashed build
  was hardware-verified for raw MJPEG delivery over 10 seconds (613,270 B,
  52 complete JPEGs) and a responsive camera-status request during streaming;
  browser rendering and flash-off behavior remain unverified.
- **Live View stream reconnection (2026-11-22).** When the MJPEG stream
  fails (image error, idle timeout, or server close), the UI now
  automatically retries with capped exponential back-off (2 s → 4 s →
  8 s → 16 s → 30 s, max 5 attempts) instead of staying on “Stream
  unavailable” or “No frames received” until the user navigates away and
  back. The image src is cleared before each retry so the browser drops any
  failed response.
- **Live View stall detection and recovery (2026-11-22).** The UI now polls
  the `` element every 250 ms and watches for a 10 s silent stall
  after frames have started flowing. On stall it stops the stream and
  triggers reconnection instead of staying on “Live” forever. A manual
  “Retry stream” button appears when the stream is in a failed state.
- **Camera frame buffers increased 2→3 (2026-11-22).** The R2 driver now
  allocates 3 PSRAM frame buffers instead of 2, giving the capture task a
  spare driver buffer when a slow stream consumer holds a lease across
  blocking socket sends. Eliminates the “all N frame buffers held by
  consumers — waiting” back-pressure warning seen during live view.

### Added

- **Wi-Fi first-run provisioning (2026-11-21).** A device with no stored
  network credentials now exposes its setup web UI at `192.168.4.1` over
  `VisionCam-XXXXXX` (AP password `visioncam`). Authenticated users can
  submit 2.4 GHz network credentials in Settings; the firmware attempts to
  obtain an IP before staging them in retained memory. After a successful
  join, the setup AP closes and firmware automatically restarts; the reboot
  commits credentials to NVS before camera startup. A failed join leaves the
  setup AP available. Passwords are never returned or logged.

- **Embedded web UI refresh (2026-11-21).** Reworked the standalone UI into
  a responsive dark NVR console using local-only HTML/CSS/JavaScript, without
  runtime frameworks, remote assets, or credential storage. Added an
  authenticated API client with same-origin cookies, double-submit CSRF,
  bounded request concurrency, error-envelope handling, and controls for all
  currently registered firmware HTTP endpoints: auth, device diagnostics,
  camera/stream, media, recording, storage/retention, motion, MQTT/HA, and
  network storage. AI and timelapse controls are intentionally not shown
  because those routes are currently excluded from the firmware build.
  Final generated assets: **79,246 B raw / 18,786 B gzip**.

- **Phase 8 task 8.1 — network storage backend (2026-11-21).** New
  `firmware/include/http_upload_storage_backend.h` +
  `firmware/src/integrations/http_upload_storage_backend.cpp`:
  `IStorageBackend` implementation over HTTP upload (SPEC 4.9) — completed
  files only, never frames. Bounded upload queue (`upload_queue_limit`,
  max 128, drop-oldest overflow), exponential backoff (base × 2^attempts,
  cap 300 s), heap gate (free heap ≥ 64 KB and largest block ≥ 16 KB before
  TLS), delete-after-upload opt-in (default off), auth token write-only
  (never returned by GET). Publish `NetworkStorageUploadFailed` on terminal
  failure. Worker task `vc_netstore` at prio 2, stack 4096 B.
  Build impact: RAM +300 B, flash +4 KB.

- **Phase 8 task 8.2 — network storage API endpoints (2026-11-21).** Four
  new auth+CSRF endpoints (SPEC 6.9):
  `GET|PUT /integrations/network-storage`, `POST /integrations/network-storage/test`,
  `GET /integrations/network-storage/queue`. GET returns config with
  `auth_token_configured` boolean (token value never echoed). PUT performs
  partial overlay with validation (scheme `http://`/`https://`, retry
  bounds, queue limit 1..128). Test performs a real GET reachability probe
  (200 `{"reachable":true}` or 503 `code:15`).

- **AP mode fallback (2026-11-21).** When no Wi-Fi credentials are
  configured, the device starts a SoftAP (`VisionCam-XXXXXX` from the last
  3 bytes of the station MAC, WPA2 passphrase `visioncam`, IP 192.168.4.1)
  so it is directly reachable for initial setup. `WifiState::Ap` added,
  `ap_mode` added to `WifiStatus`. Camera, recording and UI all work
  locally regardless of network state.

- **Auth fix — password-change loop (2026-11-21).** `authCreateSession()`
  now derives `session.complete` from `s_creds.configured` instead of
  `authAnyCompleteSession()`. Previously `authCompleteFirstLogin()`
  revoked all sessions, leaving none marked `complete`, so every subsequent
  login demanded a password change again. Now first-login change completes
  once and subsequent logins report `password_change_required: false`.

- **Web UI overhaul (2026-11-20).** Full page overhaul by the web UI
  subagent: seven page states on every page, lazy-loaded route chunks
  (`react.lazy`), responsive breakpoints at 768 px, design-system
  components (Card, Button, Field, StatCard, StatusPill, ConfirmDialog,
  PageStates), no CDN/remote fonts/images. Bundle: 331 KB JS / 105 KB
  gzip.

### Fixed

- **Recording playback and downloads (2026-11-21).** Corrected MJPEG
  playback's multipart boundary and `movi` frame offset, and stream JPEG data
  through a fixed 8 KB storage-manager reader. Recording downloads now stream
  the indexed file size in 4 KB chunks instead of allocating up to 8 MiB and
  truncating larger AVI files. Recording controls now use distinct Start/Stop
  labels, a pulsing active indicator, synchronized state across pages, and
  inline playback loading/error feedback. Hardware playback and download
  remain **UNVERIFIED**.

- **Recording lookup after reboot (2026-11-21).** Persist the bounded
  recording index on the SD card, reload and validate it at startup, and
  continue IDs from the highest saved ID so listed recordings remain
  addressable after restart. Finalize `.tmp` segments to `.avi` on stop and
  correct the stored byte count to match the actual AVI size.

- **Setup AP health status (2026-11-21).** The health endpoint now treats
  the working provisioning AP as healthy when the camera is initialized,
  rather than reporting the device as degraded until it joins a router.

- **Auth: `complete-first-login` loop.** Login after a first-boot password
  change kept demanding another change; fixed by checking
  `s_creds.configured` when marking a session complete (see Added above).

- **CSRF cookie: comma-joined `Set-Cookie` (2026-11-21).** `httpd_resp_set_hdr()`
  APPENDS (httpd_txrx.c:195-198), it does not replace. The old code comma-joined
  session + CSRF cookies into one header; curl/browsers then stored only the
  session cookie and every state-changing request failed CSRF (403). Now two
  separate `Set-Cookie` headers; CSRF cookie is JS-readable (double-submit,
  not HttpOnly).

### Changed

- **RAM/flash after Phase 8 + web UI:** RAM 102,252 B (31.2 %), flash
  1,365,899 B (86.8 %). Both within targets (RAM < 50 %, flash < 90 %).
  `firmware/src/core/` + headers: `coreInit()` boot order (RTC ring log
  first, so a crash later in the boot cannot destroy the evidence), typed
  bounded **event bus** (depth 16, drop-oldest, dispatcher task), **RTC RAM
  ring log** (4096 B `.rtc_noinit`, crash-loop counter with health point at
  *end of boot*), **resource monitor** (heap / largest block / min heap /
  PSRAM / uptime / reset reason / task stack watermarks, thresholds 32 KB
  free / 16 KB largest internal). Evidence:
  `docs/evidence/phase1-core-boot-01..04.log`; `docs/TEST_PLAN.md` 1.1.1a–e.

- **Phase 1 task 1.1.2 — network manager (2026-11-17).** New
  `firmware/src/network/` (`network_manager.cpp`, `wifi_credentials.cpp`) +
  `firmware/include/{network_manager,wifi_credentials}.h`: ESP-IDF station
  mode with one event handler and one reconnect task owning
  `esp_wifi_connect()` — exponential backoff (1 s doubling to a 32 s cap,
  0..500 ms hardware-RNG jitter, counter reset on `IP_EVENT_STA_GOT_IP`).
  `WifiState::Connected` is entered only on a real GOT_IP (DESIGN.md's
  "real connection" rule); WifiConnected/WifiDisconnected carry RSSI /
  driver-reason codes into the bounded event bus. SNTP via `esp_netif`'s
  service (IDF 6.1 merged the old `esp_sntp` component into
  esp_netif/lwip — no new dependency), started on the first GOT_IP.
  Credentials load from NVS (`visioncam` namespace, then the Phase 0
  `ESP-CAM_MJPEG` keys as a one-time migration source); lengths only are
  ever logged (AGENTS rule 17).
  **Radio-before-camera boot order is now in code**: `main.cpp` calls
  `networkManagerInit()` before camera bring-up — the ordering that closes
  the reproduced esp32-camera ISR hazard (`docs/REFERENCE_ANALYSIS.md`).
  Build: RAM 54,416 B (16.6 %), Flash 1,072,560 B (68.2 %).

- **Phase 1 task 1.1.3 — camera manager module (2026-11-17).**
  `firmware/src/camera/camera_manager.{h,cpp}` (renamed from the Phase 0
  `camera_bringup`) gains the live settings API (`set_framesize` /
  `set_quality`, with the sensor's own maximum enforced), `jpegCapacity()`
  derived from the driver's public `resolution[]` table, the recovery
  primitives (sensor reset, PWDN power-cycle, full re-init) and rule 8's
  explicit `jpeg_buffer_size` floor for sub-VGA init sizes.
  Bring-up behaviour unchanged from the Phase 0 hardware-verified sequence.

- **Phase 1 task 1.1.4 — SPEC 9.1 recovery ladder (2026-11-17).**
  `firmware/src/camera/camera_recovery.{h,cpp}` implements SPEC 9.1 as a
  single-caller state machine driven by the frame manager's capture task:
  10 ms yield per NULL (published via `cameraRecoveryYieldMs()` so the
  caller cannot drift), ≤1 rung per 2 s, ≤10 consecutive, rung 1 = sensor
  reset ×5, rung 2 = PWDN power-cycle + full re-init ×5, counters reset on
  a valid frame, then `CameraFailed` + terminal state. Metrics count NULL
  frames, rung attempts/errors, recoveries and failures.
  Hardware: `phase1-ladder-boot-01.log` (closed-loop ladder: camera held
  genuinely dead, exactly 10 gated attempts, one terminal `CameraFailed`,
  rule-3 reset verified, no spin, no panic).

- **Phase 1 task 1.1.5 — frame manager (2026-11-17).** New
  `firmware/src/frame/frame_manager.cpp` + `firmware/include/frame_manager.h`:
  the capture task is the SOLE owner of camera frame buffers (AGENTS rule 9),
  replacing the temporary smoke-probe frame access in camera_manager.h. One
  capture task (pinned to core 1, priority 6, `VC_STACK_CAPTURE`) acquires
  frames, validates them against SPEC 4.1.1 rule 7, and distributes
  refcounted leases to up to `VC_FRAME_SUBSCRIBERS` consumers through bounded
  per-consumer queues (`VC_FRAME_QUEUE_DEPTH`, drop-oldest). A fixed pool of
  `VC_FRAME_POOL` refcounted records bounds memory; when every record is
  referenced the new frame is dropped and counted (capture never blocks).
  Each frame delivery ends with `taskYIELD()` so a fast camera cannot wedge
  the core (reproduced as "Interrupt wdt timeout on CPU1" without it).
  Hardware: `phase1-frame-boot-11.log` — leases acquired/released cleanly,
  step 14 camera+SD under radio TX stable, ladder runs through the capture
  task. Build: RAM 54,784 B (16.7 %), Flash 1,079,528 B (68.6 %).

- **Phase 1 task 1.2 — web server + auth + device/camera API (2026-11-17).** New
  `firmware/src/web/web_server.cpp` + `firmware/include/{web_server,auth}.h`:
  a single `esp_http_server` instance (port 80, lru_purge on, 4 KB task stack,
  6 max_open_sockets = 4 API clients + 2 stream slots, 16 max_uri_handlers)
  serving every JSON response in the `{ok,data,error,request_id}` envelope
  with a monotonic per-boot request_id (SPEC 6.0, API contract). 15 routes:
  auth (`/auth/{session,login,logout,complete-first-login,change-password}`),
  device (`/device`, `/status`, `/health`, `/metrics`, `/device/restart`),
  camera (`/camera/status`, `/camera/settings` GET/PUT,
  `/camera/capture` POST). Auth: single user
  (default admin/admin), salted iterated SHA-256 password hash (parameters
  benchmarked on-device), RAM-only sessions from the CSPRNG, HTTP-only +
  SameSite cookies, CSRF double-submit on state-changing requests, login
  throttling (5 attempts/minute), session revocation on password change —
  no credentials or tokens ever logged or returned (AGENTS rule 17).
  Network fix: lwIP is now *always* initialized (before the web server) so
  local-only mode no longer crash-loops on `tcpip "Invalid mbox"`;
  credentials are required only to connect the radio.
  Hardware evidence: `docs/evidence/auth-debug4.log` through
  `auth-debug6.log` — login returns 200 with HTTP-only+SameSite cookies,
  `/camera/capture` returns 200 with a valid JPEG (640x480) when authenticated
  and 401 without.

- **Phase 1 task 1.3 — React + TypeScript web UI (2026-11-17).** New
  `webui/` Vite + React 19 + TypeScript project. Design tokens per DESIGN.md
  §2 (dark theme, CSS variables). Design system: Button, Field, Card,
  StatCard, Row, StatusPill, Slider, ProgressBar, Thumbnail, VideoPlayer,
  ConfirmDialog, Nav. Pages: Login (with session auto-redirect), Overview,
  Live View (MJPEG stream consumer), Settings (General/Camera/Recording/
  Motion/AI/Network/Security/System sections with Coming Soon placeholders).
  AppShell with Sidebar/Header/Footer layout. Typed API client (lib/api.ts)
  with envelope parsing, CSRF double-submit, HTTP-only cookie credentials.
  Toast provider, React Router routing. Responsive: desktop + mobile/tablet
  breakpoints. Bundle: JS 333 KB / 104 KB gzip, CSS 11 KB / 2.6 KB gzip.
  `node_modules/` and `dist/` are gitignored.

- **Phase 1 task 1.4 — web asset embedding (2026-11-17).** New
  `tools/embed_web_assets.py`: gzip-compresses, content-hashes (SHA-256), and
  embeds the Vite-built web UI as C byte arrays + asset manifest.
  5 assets: index.html, favicon.svg, icons.svg, CSS, JS (106.5 KB gzipped
  from 350.7 KB raw, 69.6% compression). `firmware/src/web_assets/`
  generated C arrays + manifest (committed so the firmware builds without
  re-running the embed tool; regenerate when the web UI changes via
  `tools/embed_web_assets.py webui/dist firmware/src/web_assets`).
  `web_server.cpp`: catch-all asset handler (`/*` URI) serves compressed
  assets with Content-Encoding: gzip; unknown URIs fall back to index.html
  for client-side SPA routing. Flash: 1,248,540 bytes (79.4%) — 324 KB
  headroom remaining.

- **Phase 2 task 2.1 — storage manager (2026-11-18).** New
  `firmware/src/storage/` — the sole filesystem owner (AGENTS rule 10):
  `IStorageBackend` interface (`storage_backend.h`) + `SdStorageBackend`
  (FATFS C stdio over the already-mounted `/sdcard` from `sd_bringup.cpp`),
  `StorageManager` (`storage_manager.cpp`/`storage_manager.h`): opaque media
  IDs (`med-…`, `evt-…`, `rec-…`), append-only JSONL media/event indexes,
  bounded list/lookup/pagination, retention policy get/set, delete, path
  traversal rejection. Web API (single `/storage` dispatcher via `?action=`
  param to stay within the 16 URI-handler limit): `status` (capacity/mount
  state), `files` (paginated listing), `file` (metadata GET, delete DELETE+CSRF),
  `rebuild` (rebuild indexes POST+CSRF), `retention` (read GET / write PUT+CSRF).
  All routes require auth cookie; state-changing routes require CSRF
  double-submit. Retention is persisted to NVS namespace `"storage"` (schema
  version 1), loaded before SD mount, survives reboot.
  Hardware verified: SD mounts 1-bit, FAT32 volume 8,051,032,064 B usable
  (99% of CSD); all endpoints return proper HTTP status codes (200 with auth
  cookie, 401 without, 404 for unknown media, 403 without CSRF, 400 for
  invalid retention values); PUT `/storage?action=retention` returns the
  sanitised effective policy, not `{"updated":true}`; retention survives a
  reboot. Evidence: `docs/evidence/phase2-storage-boot-01.log`,
  `docs/evidence/phase2-storage-boot-02.log`,
  `docs/evidence/phase2-http-transcript.log`,
  `docs/evidence/phase2-snapshot.jpg`. Build: RAM 56,648 B (17.3%),
  Flash 1,267,720 B (80.6%).

- **Phase 2 task 2.2 — media index correctness (2026-11-19).**
  Fixed three defects found during hardware verification:
  - *JSONL records had no trailing newline*: `appendMediaIndexLine` /
    `appendEventIndexLine` now write `\n` after every record (fixes
    "2154 B of index reporting media=1"). Line max raised 512 → 768 B to
    cover the structural record max (~610 B incl. escaped path); call sites
    refuse to append a truncated line.
  - *Per-line isolation*: all three JSONL split loops
    (`listEvents`, media, event in `loadIndexes`) now NUL-terminate each line
    before parsing so `strstr()` field extractors can't inherit from the next
    record.
  - *Per-line validation*: `parseMediaRecord` / `parseEventRecord` now reject
    lines shorter than 3 chars, without `{}`, or without an exact
    `med-<digits>`/`evt-<digits>` ID (new `isMediaId`/`isEventId` helpers).
    A malformed trailing record is ignored on recovery.
  - *One-shot migration*: `indexFileIsLegacy()` detects pre-newline files
    (`}{` is impossible in valid JSONL) and triggers `rebuildIndex()`.
  - *Rebuild uses file mtime*: `created_at` stamped from the file's mtime,
    falling back to wall clock only for epoch mtimes — retention age maths no
    longer treats old files as new.
  - *Rebuild preserves opaque media IDs*: path → media_id mapping carried
    over from the previous index, so a rebuild never renumbers. Verified:
    "ID mapping preserved across rebuild (12 records)".
  - *0-byte epoch files skipped*: `scanDirForMedia` skips `fsize == 0` with a
    warning (left on the card, never indexed).
  - *Boot-time self-test*: `storageSelfTestIndexParsing()` runs 9 cases
    (good record, torn write, ID-less JSON, garbage, bad ID shape, empty
    line) on string literals — no file I/O; result logged, never blocks boot.
  Evidence: `docs/evidence/phase2_full.log`; `docs/TEST_PLAN.md` 2.2.1–2.2.5.
  Build: RAM 56,680 B (17.3%), Flash 1,288,548 B (81.9%).

- **Phase 2 task 2.3 — snapshots (2026-11-19).**
  - `SnapshotManager::doCapture()` refuses epoch-dated snapshots when
    `wallClockSeconds() < kWallClockEraThreshold` (1600000000), logging and
    returning `ESP_ERR_INVALID_STATE` instead of writing a 1970/01/01 path.
    `POST /snapshot` pre-checks and returns `ErrCode::TimeUnsynchronised`.
  - Fresh frame subscriber per capture in `doCapture()`; `init()` no longer
    subscribes. JPEG buffer heap-allocated (512 KB), never on the 8 KB HTTP
    task stack.
  - Cooldown spacing 2.5 s > `cooldown_ms = 2000`; suppressed responses are
    `{"suppressed":true,"reason":"cooldown"}`.
  - All 4 snapshots fetched 200 with exact byte counts before and after
    rebuild.
  Evidence: `docs/evidence/phase2_full.log`; `docs/TEST_PLAN.md` 2.3.1–2.3.4.

- **Phase 2 task 2.4 — retention endpoints (2026-11-19).**
  - `GET /storage?action=retention-preview` — bounded preview (32 entries) of
    deletion candidates; skips non-"ready" records (active/open segments,
    active model copies never deleted). Heap-allocated candidate array to keep
    the 8 KB HTTP task stack clear.
  - `POST /storage?action=retention-run` — executes retention, returns
    `{"deleted":N,"schema_version":1}`; CSRF-protected (403 without token);
    logs every deletion (media_id, age, category).
  - `PUT /storage?action=retention` — partial PUT overlays fields on the
    current policy; range-validated in the manager; returns the sanitised
    effective policy.
  Evidence: `docs/evidence/phase2_full.log`; `docs/TEST_PLAN.md` 2.4.1–2.4.4.

- **Phase 2 task 2.5 — UI Snapshots + Storage pages (2026-11-19).**
  - New `webui/src/pages/Snapshots.tsx`: filter pills (All/Manual/Motion/
    Detection/Timelapse), date range, 5-column responsive gallery, thumbnail
    viewer, download, delete behind `ConfirmDialog` (typed "DELETE"),
    pagination ("Load more"), all seven page states.
  - New `webui/src/pages/Storage.tsx`: capacity bar (total/used/free GB/MB),
    media count, retention policy form (days per category + max_used_percent +
    enabled), retention preview list, Rebuild index + Run retention
    confirmations.
  - API client additions in `webui/src/lib/api.ts`: `getMediaList`,
    `getMediaMetadata`, `downloadMedia`, `deleteMedia`, `rebuildIndex`,
    `getRetention`, `putRetention`, `previewRetention`, `runRetention`,
    `getStorageStatus`.
  - Routes `/snapshots` and `/storage` registered in `webui/src/app/routes.tsx`.
  - Vite + tsc clean: JS 344 KB / 106 KB gzip, CSS 11 KB / 2.6 KB gzip.
  Evidence: `docs/TEST_PLAN.md` 2.5.1–2.5.5.

- **Phase 3 task 3.1 — recording state machine (2026-11-19).** New
  `firmware/src/recording/recording_manager.{h,cpp}` +
  `firmware/include/recording_manager.h`: segment-based recording with
  opaque recording IDs (`rec-…`), state machine (Idle/Recording/Closing),
  config get/set with NVS persistence (namespace `"recording"`, schema 1),
  start/stop manual recording, paginated list, lookup, delete (refuses
  active "writing" segments), and a crash-recovery scanner stub for `*.tmp`
  segments (SPEC 4.6 item 8). Writer task (prio 5) is the sole file
  writer for recordings — pulls frame leases, copies JPEG to 8 KB staging,
  releases lease immediately (never holds across blocking I/O).

- **Phase 3 task 3.1 — AVI writer (2026-11-19).** New
  `firmware/src/recording/avi_writer.{h,cpp}`: RIFF/AVI segment builder
  written fresh from the public AVI/RIFF spec (never R1's 310-byte header
  table — clean-room licence rule). Provisional 232-byte header at segment
  open; `00dc` JPEG chunks with 4-byte padding; `idx1` index (16 bytes/
  frame) in PSRAM; measured FPS stored in header at close; segment rotates
  at duration cap (60 s default) or frame cap (7200). Sector-aligned 8 KB
  staging writes to SD.

- **Phase 3 task 3.4 — playback (2026-11-19).** New
  `firmware/src/recording/playback.{h,cpp}` +
  `firmware/include/playback.h`: server-side MJPEG replay from SD — reads
  AVI in sector-multiple blocks, walks `00dc` chunks, streams
  `multipart/x-mixed-replace` at recorded FPS. Paces with
  `vTaskDelayUntil`. Runs at prio 4 (below writer at 5), `503 PLAYBACK_BUSY`
  when throughput share exceeded. Consumes one stream slot.

- **Phase 3 task 3.3/3.5 — recording API + UI (2026-11-19).** New web
  endpoints: `GET /recording/status`, `GET|PUT /recording/config`,
  `POST /recording/start`, `POST /recording/stop`,
  `GET /recordings` (paginated list + `?action=metadata|download|play` +
  `?action=delete`). All auth + CSRF protected. New `webui/src/pages/
  Recordings.tsx`: start/stop recording, paginated list with filter pills
  (All/Continuous/Manual/Event), play/download/delete actions, all seven
  page states. API client additions in `webui/src/lib/api.ts`.
  Vite + tsc clean: JS 348 KB / 107 KB gzip.

- **Phase 4 task 4.1 — motion engine (2026-11-19).** New
  `firmware/src/motion/motion_manager.{h,cpp}` +
  `firmware/include/motion_manager.h`: lightweight motion detection using
  esp_jpeg_decode to 96x96 grayscale, per-pixel SAD comparison with previous
  sample, changed-pixel ratio thresholding, two sample rates (idle ~5 Hz /
  recording 1 check/2 s), cooldown, settle/warm-up frames, light level
  derivation, and MotionStarted/MotionEnded events on the typed event bus.
  Motion runs as a FreeRTOS task (prio 4) subscribing to frame leases —
  never holds a lease across blocking I/O. Detection aborted above SXGA.
  Config persisted to NVS (namespace `"motion"`, schema 1).

- **Phase 4 task 4.2 — motion API + UI (2026-11-19).** New web endpoints:
  `GET /motion/config`, `PUT /motion/config`, `GET /motion/status`. All auth
  + CSRF protected. New Motion section in Settings page: config form
  (threshold, min changed ratio, cooldown, min frames, sample rates,
  record/snapshot/MQTT toggles) + status display (active state, events,
  light level, changed ratio). API client additions in `webui/src/lib/api.ts`.
  Vite + tsc clean: JS 352 KB / 108 KB gzip.

- **Phase 5 task 5.1-5.4 — timelapse scheduler + project manager (2026-11-19).** New
  `firmware/src/timelapse/timelapse_manager.{h,cpp}` +
  `firmware/include/timelapse_manager.h`: scheduled JPEG capture into
  per-project AVI (direct-to-AVI default) using the Phase 3 AVI writer code.
  Projects with `project.json` metadata (name, state, interval, schedule,
  resolution, quality, playback FPS, retention, counters, timestamps).
  Opaque project IDs (`tl-…`) via API. Time-synchronisation gate (refuses
  to start before clock set). Mutual exclusion with continuous recording.
  Timelapse task (prio 3) subscribes to frame leases; releases immediately.
  NVS persistence (namespace "timelapse", schema 1).
  API: `GET/POST/DELETE /timelapse/projects` (+`?action=create|start|stop`
  params). All auth + CSRF protected.

- **Phase 6 task 6.6 — TFLite Micro on-device AI (2026-11-19).** New
  `firmware/src/ai/tflite_micro_provider.{h,cpp}` +
  `firmware/include/tflite_micro_provider.h`: TFLite Micro provider class
  with model staging (`modelUploadStage`), validation (`modelValidate` —
  checks TFLM flatbuffer magic number), activation (`modelActivate` — moves
  staged to `/models/active/`, backs up previous to `/models/backup/`),
  rollback (`modelRollback` — restores from backup), and deletion
  (`modelDelete`). All TFLM-specific operations return
  `ESP_ERR_NOT_SUPPORTED` until the owner links the
  tensorflow-lite-micro component (owner decision D4 pending). Model files
  live under `/models/{staging,active,backup}/` per SPEC §4.8.
  AI API: `POST /ai/models/upload`, `/ai/models/validate`,
  `/ai/models/activate`, `/ai/models/deactivate`, `/ai/models/rollback` —
  all auth + CSRF protected. Model upload streams to staging file in
  bounded chunks (never buffers whole model in RAM, 2 MB hard cap).

- **Phase 6 task 6.2-6.4 — External AI provider + API (2026-11-19).** New
  `firmware/src/ai/external_ai_provider.{h,cpp}` +
  `firmware/include/external_ai_provider.h`: External AI provider using
  ESP-IDF's esp_http_client to POST JPEG frames to an external inference
  server (`POST /api/v1/infer` multipart/form-data). Configurable base
  URL, auth token, timeout, upload resolution/quality, interval, TLS
  verification. Never holds frame leases across I/O (SPEC 4.1.1 rule 9).
  Returns `ESP_ERR_NOT_SUPPORTED` until owner adds the esp-http-client
  dependency (already available in ESP-IDF).
  AI API endpoints: `GET /ai/status`, `GET /ai/providers`, `GET|PUT
  /ai/config`, `GET /ai/models`, `GET /ai/events`,
  `POST /ai/models/upload`, `POST /ai/models/validate`,
  `POST /ai/models/activate`, `POST /ai/models/deactivate`,
  `POST /ai/models/rollback` — all auth + CSRF protected.
  AI Settings UI page with provider selection, external AI config form,
  model management actions. Vite + tsc clean: JS 352 KB / 108 KB gzip.

### Fixed

- **Every boot crash-looped with PANIC (phase1-web-boot-01.log, 259 boots).**
  Root cause: `networkManagerInit()` skipped `esp_netif_init()` when no
  credentials existed, so lwIP/tcpip never ran and the web server assert-failed
  on `tcpip_send_msg_wait_sem (Invalid mbox)`. lwIP is now initialized
  unconditionally; the radio only connects when credentials exist.

### Fixed

- **Network credential fallback was unreachable (2026-11-17).**
  `wifiCredentialsLoad` compared `nvs_get_str`'s NVS-specific
  `ESP_ERR_NVS_NOT_FOUND` against the generic `ESP_ERR_NOT_FOUND`
  (different numbers), so "key missing" in our own namespace propagated as
  a fatal error and the legacy migration fallback never ran. Exposed by
  `docs/evidence/phase1-net-boot-02.log`.
- **`esp_wifi_sta_get_rssi` takes `int*`**, not `int8_t*` (build fix).

### Changed

- Smoke step 14 now branches on `networkManagerRadioOwned()`: with
  credentials the manager's real association traffic is the TX load (no
  SoftAP next to a live STA); without them the Phase 0 SoftAP path runs in
  the hazard-safe camera-down → radio-up → camera-up order.
- Verdict line gains a `net:` column (`Connecting`/`Connected`/`Off`).

### Changed

- **Web layer fixes (2026-11-18).**
  - `jsonString` → `jsonScalar` in `web_server.cpp`: the old quoted-only
    parser silently failed on JSON numbers, so PUTs reported success while
    writing nothing (retention PUT never persisted). `jsonScalar` accepts
    quoted strings, bare numbers and bare literals; rejects nested `{`/`[`.
    11 call sites renamed.
  - `httpReplyError` now sets the HTTP status per `ErrCode`: 401
    Unauthenticated/SessionExpired, 403 CsrfInvalid, 400 ValidationFailed,
    404 NotFound, 429 RateLimited, 503 SdNotMounted/StreamSlotLimit/
    PlaybackBusy/TimeUnsynchronised, 507 SdFull, 500 SdWriteFailed/Internal.
    Contract change to be logged at release.
  - Frame-manager flush hook: `cameraManagerSetFlushHook()` is called inside
    `cameraManagerShutdown()` right after `esp_camera_deinit()` (while
    `s_state` is NULL, so `esp_camera_fb_return` is a safe no-op); the frame
    manager registers `flushQueuesOnCameraShutdown()` at init to drain all
    subscriber queues so no record crosses a camera lifetime.
  - Capture handler's auto `frameManagerStart()` removed — resurrecting the
    capture task during boot-test stop windows raced `esp_camera_deinit()`
    (panics documented at `main.cpp:409-413`); `frameAcquire` now returns
    "no frame available" instead.

### Known issues / owner actions

- **The board's LAN credentials were lost** (NVS erase after a
  tool/firmware partition-view mismatch — root cause, rule and recovery in
  `docs/REFERENCE_ANALYSIS.md` and `tools/nvs_provision/README.md`). The
  owner must re-provision (workstation → hotspot → `set`) before the next
  connection test. Station-mode behaviour itself is already proven
  (`phase1-net-boot-01.log`: GOT_IP 192.168.43.171, rssi −68 dBm, backoff
  retry at 2 s).
- **SNTP sync completion is `UNVERIFIED`** — the sync line fell outside the
  capture window; re-test with a longer capture once credentials return.

- **Phase 1 task 1.1.1 — core services (2026-11-17).** New
  `firmware/src/core/` + headers: `coreInit()` boot order (RTC ring log
  first, so a crash later in the boot cannot destroy the evidence), typed
  bounded **event bus** (depth 16, drop-oldest, dispatcher task), **RTC RAM
  ring log** (4096 B `.rtc_noinit`, crash-loop counter with health point at
  *end of boot*), **resource monitor** (heap / largest block / min heap /
  PSRAM / uptime / reset reason / task stack watermarks, thresholds 32 KB
  free / 16 KB largest internal). Evidence:
  `docs/evidence/phase1-core-boot-01..04.log`; `docs/TEST_PLAN.md` 1.1.1a–e.
- **Two hardware findings recorded in `docs/REFERENCE_ANALYSIS.md`
  (2026-11-17):**
  (1) *esp32-camera 2.1.9 ISR hazard* — starting the Wi-Fi radio while the
  camera streams overflows the camera's 1–3-deep DMA event queue; the ISR's
  overflow warning reads a flash-resident format string while the flash
  cache is off (radio-init NVS reads) → `Cache error` panic, reproduced
  **3× byte-identical** (`phase1-core-boot-03.log`). Upstream `master` has
  the same unfixed macro, so no pin bump helps and patching
  `managed_components/` is forbidden (rule 33). Mitigations adopted:
  network-before-camera boot order, never start the radio while the camera
  streams (smoke step 14 now does camera-down → `wifiStart()` → camera-up —
  verified clean in `phase1-core-boot-04.log`), serialise NVS writes with
  camera framesize reconfig (task 1.2), short frame leases (task 1.1.5).
  (2) *RTC noinit retention* — EN-pin resets (upload / manual reset) clear
  `.rtc_noinit`; SW/PANIC resets preserve it (proven 3×). Operational rule:
  `boot=1` after a flash is **not** broken retention.
- **Crash-loop blind spot found and fixed:** health used to be marked when
  the camera came up (~0.8 s) but crashes occurred later (~2.2 s), so
  `abnormal_boots` stayed 0; `ringLogMarkHealthy()` moved to after the boot
  verdict (`phase1-core-boot-03.log` vs `-04.log`).

- **Phase 0 closed — all owner questions answered (2026-11-16).** The three
  D2 follow-ups are recorded with evidence: **GPIO4 lamp under R1 observed**
  (boot-window flash consistent with Arduino `SD_MMC` initialising pin 4 at
  mount `00:00:00.572` — before R1's own lamp feature exists at `00:00:01.277`,
  so the later repeating blink is R1's status pattern, not SD activity; the
  ESP-IDF driver still provably leaves the lamp off for the whole boot —
  `docs/evidence/phase0-d2-r1-lamp-run.log`); **D5 answered** (genuine
  SanDisk 8 GB, no speed-class marking visible; card reformatted the same
  day); **weak-supply brownout drill skipped by owner decision**
  (normal-supply behaviour verified brownout-free). `docs/HARDWARE.md` §1/§3.6/§4,
  `docs/TEST_PLAN.md` 0.11, plans updated.
- **`tools/sd_format/` — one-shot SD card formatter (2026-11-16).** The
  owner's 8 GB SanDisk card shipped with a bootable Linux Mint image (the
  "5 MB FAT volume" of `docs/HARDWARE.md` §4.2 was its EFI partition; the
  rest was ext4, invisible to FATFS). VisionCam never formats a card
  (SPEC §2.3 keeps `format_if_mount_failed = false`), so the rewrite lives
  in this standalone tool: flash it, run `format`, type `ERASE`
  (case-sensitive, nothing is written before it), then flash the firmware
  back. FatFs `f_mkfs` from the pinned ESP-IDF 6.1.0 wrote **MBR (one
  partition, `0x0C`, start LBA 63) + FAT32 across the whole card:
  8,051,032,064 B usable = 99 % of the CSD**, 32 KB clusters, format
  2,791 ms, write/read/delete self-test OK. Evidence:
  `docs/evidence/phase0-sd-format.log`; VisionCam reflash re-mounts it with
  no coverage warning and a green verdict line
  (`docs/evidence/phase0-smoke-boot-12-postformat.log`); TEST_PLAN row 0.13;
  analysis in `docs/HARDWARE.md` §4.2.1 and the `docs/REFERENCE_ANALYSIS.md`
  f_mkfs entry. Clean-room (D1): no R1 code.
- **D2 R1 baseline — measured 2026-11-15.** R1 (`ESP32-CAM_MJPEG2SD`
  10.10.1 @ `abe84d8`) was flashed **unmodified** and driven over HTTP while a
  passive 115200 capture recorded its own stats block at every recording close.
  `docs/HARDWARE.md` §4 now holds the acceptance bar: recording FPS 30.8
  (QVGA), 20.6 (VGA), 20.4 (SVGA), 5.3 (XGA), 6.2 (SXGA) at quality 12, plus
  VGA at quality 8 (20.7) and 16 (20.4); SD write 493–520 kB/s
  (0.48–0.51 MB/s); free heap 72–75 KB, largest block 41–43 KB, lifetime min
  53,504 B, pSRAM free 914,892 B; zero brownout/panic/WDT events. Evidence:
  `docs/evidence/phase0-d2-r1-avi-stats-blocks.txt`,
  `docs/evidence/phase0-d2-r1-serial-run.txt`.
- **`tools/nvs_provision/`** — small pinned ESP-IDF helper (no R1 code) that
  writes R1's two documented Preferences keys (`ST_SSID`/`ST_Pass` in namespace
  `ESP-CAM_MJPEG`) so R1 joins the owner's LAN and the baseline can be measured
  from this workstation instead of a second device. Reads the passphrase from
  NetworkManager over UART; it is never printed, logged or put on a command
  line (rule 17) and nothing credential-bearing is committed. Also carries a
  `join` diagnostic that prints the raw `WIFI_REASON_*` code — which is how
  R1's first post-provision failure was shown to be a transient association
  issue, not bad credentials. Procedure: `docs/HARDWARE.md` §3.7.
- **FAT volume diagnostic in the SD smoke test** — `sd_bringup.cpp` now prints
  the mounted volume (usable/free bytes, cluster layout) next to the CSD and
  warns when the volume is under one eighth of the CSD capacity.
- **Storage finding** (`docs/HARDWARE.md` §4.2): the owner's card reports
  **CSD 8,053,063,680 B** and CID product name **`SD16G`**, but both FATFS
  implementations (Arduino's and IDF's) report a **5,216,256 B FAT12 volume
  with 1,349,632 B free** — the formatted volume does not cover the card.
  D5 and Phase 9 retention sizing stay open on it.

- **Analysis and planning** (`.opencode/plans/`)
  - `00-analysis.md` — inventory, 15 hard constraints, authority order,
    8 in-spec supersessions, mockup and reference analysis, gaps G1–G16,
    risks, traceability map, and a 34-row spot-check of SPEC claims against
    the pinned R1/R2 source.
  - `README.md`, `owner-decisions.md`, `11-acceptance-matrix.md`.
  - Ten phase work orders: `01-phase-0-baseline.md` …
    `10-phase-9-hardening-release.md`.
- **Agent instructions** — root `AGENTS.md` (SPEC §14's 37 rules), scoped
  `firmware/AGENTS.md` and `webui/AGENTS.md`, root `SKILL.md` pointer, and six
  skills under `.opencode/skills/` (`visioncam-dev`, `visioncam-build-flash`,
  `visioncam-reference-first`, `visioncam-webui`, `visioncam-api-contract`,
  `visioncam-hardware-validation`).
- **Owner decisions D1, D2, D3** recorded (clean-room licence; R1 baseline run
  during Phase 0; FPS parity tolerance **10 %**).
- **Reference pinning** — `reference/README.md`, `reference/pins.txt` (tree
  SHA-256 + upstream commit for both refs), `reference/fetch_references.sh`
  with `--verify`.
- **Repository baseline** — `git init`, `.gitignore`, directory skeleton,
  initial commit `edfbb1e`.
- **Phase 0 build** — `platformio.ini`, root + component `CMakeLists.txt`,
  `sdkconfig.defaults`, `firmware/include/board_profile.h`,
  `firmware/src/main.cpp` smoke test.
- **Pinned IDF components** (task 0.2) — `firmware/src/idf_component.yml`
  (`espressif/esp32-camera@2.1.9`, `espressif/mqtt@1.1.0`), committed
  `dependencies.lock` (also pins transitive `espressif/esp_jpeg@1.3.2`),
  `managed_components/` gitignored.
- **Rule 33 single-copy check** in the root `CMakeLists.txt` — fails the
  configure if zero or more than one component provides `esp_camera.h`.
  Both the pass and the fail path were exercised with a real fixture.
- **Camera + SD smoke modules** (task 0.3 steps 11–13) —
  `camera_bringup.{h,cpp}` (SPEC §4.1.1: PSRAM-sized init, 3 attempts with
  PWDN power-cycle, OV2640 PID check, ≥3 warm-ups, rule-7 frame check),
  `sd_bringup.{h,cpp}` (SPEC §2.3: SDMMC slot 1, 1-bit, no auto-format,
  write/read/delete self-test, GPIO4 lamp probe), rewritten `main.cpp`.
- **Partition table** (deliverable 5) — `partitions.csv`: two 1.5 MB OTA
  slots, 8 KB `otadata`, 4 KB `phy_init`, 92 KB `nvs`, 884 KB `storage`.
  Exact fit to `0x400000`, alignments and overlaps validated.
- **Docs** — `docs/adr/0001-toolchain.md`, `docs/HARDWARE.md` (R1 baseline
  procedure + fill-in tables), `docs/REFERENCE_ANALYSIS.md`, `README.md`,
  this file.
- **Reference analysis, first entries** (task 0.4 step 18) —
  `docs/REFERENCE_ANALYSIS.md` gains three sections: the two verified
  divergences between SPEC §2.3's mount snippet and pinned IDF 6.1.0
  (`SDMMC_HOST_FLAG_1BIT` deliberately not set; slot `clk/cmd/d0` inert on
  ESP32), pinned-R2 init/retry facts (stale "no way to de-initialize" doc
  comment, `OV2640_PID = 0x26`, `width*height/5` confirmed), and the eight
  §1A.5 matrix rows verified first-hand against the pinned R1/R2 source.
- **Wi-Fi smoke module** (task 0.3 step 14) — `wifi_bringup.{h,cpp}`: APSTA
  SoftAP `visioncam-smoke` + one blocking scan (IDF 6.1 renamed the API to
  `esp_wifi_scan_start`), no credentials anywhere, nothing joined. `main.cpp`
  runs camera + SD cycles with the radio off, then on for 3 s of beacon TX.
- **Forced first-attempt failure test** (step 11 part 2) — attempt 1 runs with
  `pin_xclk = -1` so the SCCB probe genuinely fails, the normal retry
  (`cameraShutdown()` + PWDN power-cycle) must recover on attempt 2. First
  attempt with a PWDN hold **failed the test on hardware**: R2's
  `camera_probe` releases PWDN itself (`esp_camera.c:204-216`).
- **Hardware evidence logs** — `docs/evidence/` holds the raw serial captures
  behind every claim in `docs/HARDWARE.md` §2/§4/§5.

### Fixed

- **`camera_bringup.cpp` comment overstated why `cameraShutdown()` is
  required.** A *failed* `esp_camera_init` already deinitialises via its
  `fail:` label (`esp_camera.c:374-376`); the explicit `esp_camera_deinit()`
  is what the post-success exit paths (PID mismatch, warm-up, malformed
  frame) need, or a later bring-up hits `ESP_ERR_INVALID_STATE`. Comment now
  matches the pinned source.
- **Partition table silently not reaching the flashed binary.**
  `sdkconfig.defaults` set `CONFIG_PARTITION_TABLE_CUSTOM*` correctly, but
  PlatformIO's SCons pipeline reads `board_build.partitions` instead and fell
  back to the framework's `partitions_singleapp.csv`, so
  `pio run -t upload` would have flashed a 1 MB `factory` table and placed the
  app at `0x10000` — inside our NVS region. Added
  `board_build.partitions = partitions.csv`; both pipelines now produce
  byte-identical tables (md5 `7ca60f29f1fa15cf0b949d746387d5da`) and the app
  offset resolves to `0x20000`. See `docs/adr/0001-toolchain.md`.

### Notes

- **Phase 0 smoke test VERIFIED on hardware 2026-11-15** (owner's board,
  evidence in `docs/evidence/`): flash + auto-reset, `POWERON_RESET` banner,
  **PSRAM 8,388,608 B** (4 MB mapped), OV2640 PID `0x0026` + 243,471 B JPEG,
  forced-failure recovery at `attempts=2`, SD 1-bit mount + self-test at
  20,000 kHz, **GPIO4 lamp OFF for the whole boot**, Wi-Fi off/on cycles
  (`step 14 result: OK`, 2 networks heard), single boot, no
  brownout/panic/WDT. Verdict line: `camera: OK | sd: OK | psram: OK |
  wifi(step14): OK | forced-retry: OK`. Recorded in `docs/HARDWARE.md` §5.
- **D2 R1 baseline recorded** — measurements are **not** `UNVERIFIED` any more
  (`docs/HARDWARE.md` §4). The three items owed to the owner's eyes/hands
  (`owner-decisions.md` D2) were asked for in one message and are **all closed
  2026-11-16** (see the Phase 0 closure entry above): D5 answered (genuine
  SanDisk 8 GB, no class marking), GPIO4 lamp observed under R1, weak-supply
  drill skipped by owner decision. Phase 0's own phase-end
  stack-HWM/FPS row stays `UNVERIFIED` until a capture loop exists (Phase 1+).
- **Two R1 behaviours recorded as rejected** (`docs/REFERENCE_ANALYSIS.md`):
  `checkFreeStorage()`'s oldest-folder deletion loop, which on a volume
  smaller than `sdMinCardFreeSpace` can never exit (it was disarmed on the
  owner's card before any recording, in warn-only mode); and R1's AVI stats
  block, which counts bytes it *attempted* to write — on a full disk it
  reported `File size: 12.3MB` / `3796 kB/s` for a file that was really
  1,349,632 B. Phase 3's recorder must check every write's return value.
- **Hardware quirks found:** SD mount errors varied (`ESP_ERR_TIMEOUT` /
  `ESP_ERR_INVALID_RESPONSE`) until the card was re-seated — contact, not
  firmware (`docs/HARDWARE.md` §5.1); IDF maps only 4 MB of the 8 MB PSRAM;
  the ESP-IDF SDMMC driver does **not** light GPIO4 (R1's workaround is
  Arduino-`SD_MMC`-specific).
- Build sizes with Wi-Fi: RAM 15.4 % (50,584 / 327,680 B), Flash 1,041,544 B
  = 66.2 % of the 1.5 MB `ota_0` slot — Wi-Fi + mbedTLS + supplicant cost
  ≈654 KB flash over the no-radio build.
- All six `sdkconfig.defaults` symbols confirmed effective in the generated
  `sdkconfig.h`; PSRAM defaults to 40 MHz and flash to 2 MB in IDF 6.1.0, so
  both are overridden explicitly.
- Two build failures diagnosed and documented in ADR-0001: missing
  `PRIV_REQUIRES esp_psram` (renamed `main` component gets no automatic
  dependencies) and `esp_psram.c` being compiled only under `CONFIG_SPIRAM`.
- Log-level trap documented in `sdkconfig.defaults`: with
  `CONFIG_LOG_MAXIMUM_LEVEL_WARN` every `ESP_LOGI` is compiled out and
  `esp_log_level_set()` cannot restore it — the maximum must be INFO while
  the runtime default stays WARN.
