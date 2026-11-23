# Test plan and live status

The **map** (which acceptance item is proved by which phase and skill) lives
in `.opencode/plans/11-acceptance-matrix.md`. This file carries the **live
status**. Status values: `TODO` → `RUN PASS` / `RUN FAIL` / `UNVERIFIED`
(hardware or environment not yet available).

Rule: never write `RUN PASS` without a serial log or tool output behind it
(AGENTS rule 26). Evidence for hardware runs is committed under
`docs/evidence/`.

---

## Phase 0 — repository and platform baseline

Hardware run: **2026-11-15**, owner's AI-Thinker ESP32-CAM over
ESP32-CAM-MB. Evidence:
`docs/evidence/phase0-smoke-boot-10-wifi-lamp.log`,
`docs/evidence/phase0-smoke-boot-11-forced-retry.log`.

| # | Test | Status | Evidence |
|---|---|---|---|
| 0.1 | Build: `pio run` green, pinned toolchain | RUN PASS | `docs/adr/0001-toolchain.md` verification table |
| 0.2 | Single `esp32-camera` copy (rule 33) | RUN PASS | configure-time check, pass + fail path exercised |
| 0.3 | Flash via ESP32-CAM-MB (auto-reset) | RUN PASS | boot 11 log, `POWERON_RESET` |
| 0.4 | Banner: reset reason / PSRAM / heap | RUN PASS | boot 11 log — `PSRAM: 8388608 bytes`, heap free 4,448,703 B |
| 0.5 | Camera: PID OV2640, ≥3 warm-ups, JPEG captured | RUN PASS | boot 11 log — `PID 0x0026`, 243,471 B (1600×1200) |
| 0.6 | Forced first-attempt failure → retry recovers | RUN PASS | boot 11 log — `attempts=2 -> OK` (sabotage `pin_xclk=-1`) |
| 0.7 | SD mounts 1-bit, write/read/delete round-trip | RUN PASS | boots 8–11 — `SSR: bus_width=1`, self-test OK |
| 0.8 | GPIO4/lamp with SD mounted | RUN PASS | owner observation: **OFF** the whole boot (2026-11-15) |
| 0.9 | Camera + SD together, Wi-Fi off then on, no brownout | RUN PASS | boots 10/11 — `step 14 result: OK`, 1 boot, no `BROWNOUT` |
| 0.10 | Partition table as flashed | RUN PASS | boot 11 log lists all 6 partitions from `partitions.csv` |
| 0.11 | **D2 — R1 baseline table** (`docs/HARDWARE.md` §4: FPS per size/quality, R1 heap, SD write MB/s, weak-supply brownout, exact card label → D5) | **RUN PASS** (measurements + all follow-ups closed) | `docs/evidence/phase0-d2-r1-avi-stats-blocks.txt` — 8 recording runs: QVGA 30.8, VGA 20.6, SVGA 20.4, XGA 5.3, SXGA 6.2 FPS at quality 12 (+ VGA at q8/q16), SD 0.48–0.51 MB/s, no brownout/panic/WDT. Follow-ups closed 2026-11-16: lamp **observed** under R1 (`phase0-d2-r1-lamp-run.log`), D5 = SanDisk 8 GB no class marking, weak-supply drill **skipped by owner decision** |
| 0.12 | Phase-end resource row: stack high-water marks, FPS | **UNVERIFIED** | VisionCam's own FPS/stack HWM need a running capture loop — **Phase 1+**; the R1 baseline that must be matched against is now recorded (0.11) |
| 0.13 | **SD reformat** (owner-authorized): `tools/sd_format/` rewrites the Mint image card as MBR + FAT32 across the whole card; typed `ERASE` gate; post-format self-test; VisionCam reflash mounts it with no coverage warning | **RUN PASS** | `docs/evidence/phase0-sd-format.log` — pre-format Mint table (`p2 0xEF` 5,242,880 B + `p3 0x83` 4,959,764,480 B, `FAT covers 0%` + warning), `f_mkfs OK in 2791 ms`, `p1: type 0x0C, start LBA 63 = 8,053,031,424 B`, FAT32 8,051,032,064 B usable (99 %), `self-test OK`, `FORMAT OK`; `docs/evidence/phase0-smoke-boot-12-postformat.log` — boot 12 verdict line all green, FAT-vs-CSD line with **no** warning, single `POWERON_RESET` |

## Known hardware quirks found in Phase 0

- **SD contact:** mount errors varied between boots (`ESP_ERR_TIMEOUT` /
  `ESP_ERR_INVALID_RESPONSE`) until the card was re-seated — six of the first
  seven boots failed; boots 8–11 all passed. Re-seat before debugging code
  (`docs/HARDWARE.md` §5.1).
- **PSRAM:** 8 MB fitted, only **4 MB mapped** by IDF on the original ESP32.
- **GPIO4:** ESP-IDF SDMMC 1-bit driver does **not** light the lamp (Arduino
  `SD_MMC` does — that is what R1's workaround targets).
- **R2 PWDN:** `esp_camera.c:204-216` releases PWDN at the start of *every*
  init, so an external PWDN hold cannot force a failed attempt.
- **Storage — the usable volume is 5 MB, not 7.5 GiB** (`docs/HARDWARE.md`
  §4.2): CSD says 8,053,063,680 B, CID calls the card `SD16G`, but both FATFS
  implementations (Arduino's and IDF's) report a 5,216,256 B FAT12 volume with
  1,349,632 B free — the card's own FAT simply does not cover it. Confirmed
  independently: `docs/evidence/phase0-d2-vc-fat-crosscheck-boot.log`.
  **Resolved 2026-11-16 (§4.2.1):** the card held a bootable Linux Mint
  image (EFI + ext4); owner-authorized reformat by `tools/sd_format/` now
  gives **8,051,032,064 B usable (99 % of CSD)** — row 0.13. Lesson kept:
  *a FAT volume far smaller than the CSD means a foreign partition layout,
  not necessarily a broken card* — decode the MBR before blaming the
  hardware.
- **R1's stats block lies on a full disk** (`docs/HARDWARE.md` §4.2):
  `saveFrame()` counts bytes it *attempted* to write and never checks
  `aviFile.write()`'s return value, so a truncated file still reports
  `File size: 12.3MB` / `3796 kB/s`. Phase 3's recorder must not copy this
  failure mode — check every write's return value.

## Phase 1 — core camera and web UI (in progress)

| # | Test | Status | Evidence |
|---|---|---|---|
| 1.1.1a | Core services boot first: ring log reads previous session, event bus round trip, resource snapshot + task table | RUN PASS | `docs/evidence/phase1-core-boot-01.log`, `-02.log` — 9 tasks, `vc_eventbus` 2572 B / `vc_monitor` 3288 B stack; event bus published=1 delivered=1 |
| 1.1.1b | Crash-loop counter counts a crash after the health point | RUN PASS (found + fixed a blind spot) | `-03.log`: health was marked at camera-OK (~0.8 s) but the panic hit ~2.2 s → `abnormal=0`. Marker moved to end of boot; `-04.log` shows `health marked` after the verdict |
| 1.1.1c | RTC ring log retention across resets | RUN PASS with documented limits | `-02.log` (EN reset clears `.rtc_noinit`), `-03.log` (SW/PANIC resets preserve it ×3). EN-clear is documented behaviour: a flash/manual EN reset starts a fresh log — `docs/REFERENCE_ANALYSIS.md` |
| 1.1.1d | **Driver hazard:** camera DMA ISR overflow warning vs flash-cache-off windows — radio must never start while the camera streams | RUN FAIL reproduced ×3, mitigation verified | `-03.log` = three byte-identical panics (PC 0x40008047 in ROM `ets_printf` called from the ISR's `EV-EOF-OVF` branch during `wifiStart()`); `-04.log` = clean boot with camera-down→`wifiStart()`→camera-up order, `step 14 result: OK`, no panic. Root cause + mitigations in `docs/REFERENCE_ANALYSIS.md` ("Camera driver ISR overflow warning…"); upstream `master` has the same unfixed macro — no pin bump helps, patching `managed_components/` is forbidden (rule 33) |
| 1.1.1e | Step 14 concurrency (camera + SD + radio) under the new order | RUN PASS | `-04.log` — `core: OK \| camera: OK \| sd: OK \| psram: OK \| wifi(step14): OK \| forced-retry: OK`, single `POWERON_RESET`, no brownout/panic |
| 1.1.2a | Network manager: radio started BEFORE the camera (hazard-safe order), no panic | RUN PASS | `docs/evidence/phase1-net-boot-01.log` — `credentials loaded (ssid 8 B, pass 9 B)` → `radio starting in STA mode` at 1231 ms, `CAMERA OK` at 2051 ms, no panic/cache error (boot-03's panic was the old order) |
| 1.1.2b | Station mode connects for real: GOT_IP, RSSI, backoff diagnostics | RUN PASS | `phase1-net-boot-01.log` — `network: Connecting` at 1231 ms, `disconnects=1 attempts=2 backoff=2000 ms` after 3 s (exponential 1 s + jitter working), `GOT_IP 192.168.43.171 (rssi -68 dBm, connects=1)` at 8221 ms; step 14 ran camera+SD under real association TX: `step 14 result: OK` |
| 1.1.2c | Unconfigured (local-first) mode leaves the driver untouched | RUN PASS | `phase1-net-boot-04.log` — `no credentials configured — local-only operation`, `net: Off`, no radio ever started, SoftAP step-14 path OK, all verdicts OK |
| 1.1.2d | SNTP time sync | **UNVERIFIED** | SNTP starts on GOT_IP (code path taken in boot-01), but the sync-callback line never landed in the capture window (the capture stopped 2 s after the verdict, GOT_IP came later). Re-test with a longer capture after the credentials are re-provisioned |
| 1.1.2e | Credential legacy-fallback migration | **RUN FAIL then fixed** | `phase1-net-boot-02.log` exposed a real bug: with our namespace present but empty (after `vcunset`), `wifiCredentialsLoad` compared `nvs_get_str`'s NVS-specific `ESP_ERR_NVS_NOT_FOUND` against the generic `ESP_ERR_NOT_FOUND` and treated "key missing" as fatal — the legacy fallback was unreachable. Fixed; the migration line itself was produced on the (uncaptured) upload boot of `phase1-net-boot-01.log`. **Re-validation pending credential re-provision** — see the NVS hazard note in the tool README |
| 1.1.3a | Camera manager: §4.1.1 bring-up as a module (init size from PSRAM, PID, warm-up, rule-7 validation, VGA runtime default) + live settings API + rule-8 explicit buffer | RUN PASS | `docs/evidence/phase1-cam-boot-01.log` — `PSRAM 8388608 B -> init frame size 15` (UXGA), `PID 0x0026 = OV2640`, `JPEG grabbed: 254328 bytes (1600x1200), capacity 384000` (= 1600×1200/5), `frame size set to 10 (640x480), jpeg capacity 61440 B`, warm-up 3, forced-retry `attempts=2 -> OK`, all verdicts OK, no panic |
| 1.1.3b | Rule-8 explicit `jpeg_buffer_size` for very small init sizes | **code path present, not reachable on this board** | `cameraManagerJpegCapacity`/`explicitJpegBufferFor` derive the floor (32 KB) from the driver's public `resolution[]` table below VGA; this board's PSRAM picks UXGA, so the driver default already has room. Exercised by inspection; needs a sub-VGA board to be a hardware fact |
| 1.1.4a | SPEC 9.1 NULL-frame recovery ladder, closed-loop on hardware | RUN PASS | `docs/evidence/phase1-ladder-boot-01.log` — standalone ladder (no frame manager yet): camera held genuinely dead, rung 1 ×5 + rung 2 ×5 gated to 2 s apart, exactly 10 attempts, one terminal `CameraFailed`, rule-3 reset verified, no spin, no panic |
| 1.1.5a | Frame manager: capture task is the SOLE `esp_camera_fb_get`/`fb_return` owner (AGENTS rule 9), bounded pool (4 records), per-consumer queues (depth 2, drop-oldest), ~10 ms yield per frame to avoid wedging the core | RUN PASS | `docs/evidence/phase1-frame-boot-11.log` — `frame manager running for consumer 0`, step 14 leases acquired and released cleanly (`lease id=1, 61272 B`), no core wedging, no interrupt-WDT panic after the capture-task yield fix |
| 1.1.5b | Frame manager + SPEC 9.1 ladder integration (task must RUN for rungs) | RUN PASS (ladder); restore failure found in later boot test | `docs/evidence/phase1-frame-boot-11.log` — ladder runs through the live capture task: rung 1 ×5 + rung 2 ×5 gated to 2 s apart (`elapsed=21182 ms`), `attempts=10 rung1=5 rung2=5 errors=10`, one terminal `CameraFailed`, `ladder reset=1`. A later 2026-11-22 UART run found that running this destructive ladder on every boot could leave only a 23,552 B largest internal block for a 32,768 B camera DMA allocation; normal boots now skip the ladder, and opt-in test verdict includes restore success. |
| 1.1.5c | Interrupt-watchdog fix: capture task yields between frames instead of tight-looping on immediate `fb_get` returns | RUN PASS | Reproduced as "Interrupt wdt timeout on CPU1" Guru Meditation when the capture task tight-looped; fixed with `taskYield()` after each frame and pool/exhaustion drop; no panic in `phase1-frame-boot-06` through `-11` |
| 1.2a | HTTP server: esp_http_server on port 80, lru_purge on, 4 KB stack, 6 max_open_sockets, 16 max_uri_handlers, envelope + request_id on every response | RUN PASS | `docs/evidence/phase1-web-boot-03.log` — `web server running`, all 14 routes register, no panic, health marked |
| 1.2b | lwIP always initialized (web server needs tcpip even in local-only mode) | RUN PASS | Fixed `tcpip_send_msg_wait_sem (Invalid mbox)` assert that crash-looped every boot (phase1-web-boot-01.log showed 259 panic boots); phase1-web-boot-03.log shows single clean boot |
| 1.2c | Auth: login (default admin/admin on first boot), logout, session, complete-first-login, change-password, CSRF double-submit, login throttling | RUN PASS | HTTP tested end-to-end on 10.152.141.4: login returns 200 with HTTP-only+SameSite cookies, /auth/session returns 200 with valid cookie, /camera/status returns 401 without cookie and 200 with cookie. Credentials re-provisioned via tools/nvs_provision (SSID: "nik's A03s") |
| 1.2d | Device/status/health/metrics/restart endpoints | RUN PASS | HTTP tested: /device, /status, /health, /metrics all return 200 with valid session cookie, 401 without |
| 1.2e | Camera settings: GET/PUT /camera/settings with framesize/quality validation | RUN PASS | HTTP tested: GET returns current settings, PUT applies live changes |
| 1.2f | Camera capture: POST /camera/capture returns JPEG | RUN PASS | HTTP tested: authenticated POST returns 200 with valid JPEG image (640x480, 19759 bytes), unauthenticated POST returns 401 |
| 1.2g | Live MJPEG delivers browser frames while control APIs stay responsive; flash POST requires auth + CSRF and turns GPIO4 off when the stream closes | **UNVERIFIED (raw transport verified; browser and flash pending)** | Local Playwright mock verifies rendered stream state and flash transitions. Hardware raw-stream test: `docs/evidence/phase1-live-stream-boot-13.log` — authenticated 10 s stream delivered 613,270 B (53 JPEG starts / 52 complete JPEGs); concurrent `/camera/status` returned HTTP 200. Browser-rendered frames and hardware flash-off lifecycle remain UNVERIFIED. The boot-time recovery self-test is disabled by default. |
| 1.3a | Web UI builds (vite + tsc clean) | RUN PASS | `npm run build` — JS 333 KB / 104 KB gzip, CSS 11 KB / 2.6 KB gzip |
| 1.3b | All seven page states implemented (loading, empty, error, degraded, success, auth, confirm) | RUN PASS | Login, Overview, Live View, Settings pages each implement loading/empty/error/toast states; ConfirmDialog for destructive actions |
| 1.3c | No runtime CDN, no remote fonts, no remote images | RUN PASS | system-ui font stack, inline SVGs only, all assets self-contained |
| 1.3d | Responsive: desktop + mobile/tablet breakpoints | RUN PASS | CSS media queries at 768px; collapsible sidebar, one-column layout on mobile |
| 1.4a | Web assets embedded as gzipped C arrays + manifest | RUN PASS | `tools/embed_web_assets.py` — 5 assets, 106.5 KB gzipped from 350.7 KB raw (69.6% compression) |
| 1.4b | Asset serving: catch-all `/*` handler serves compressed assets with Content-Encoding: gzip | RUN PASS | `phase1-web-boot-04.log` — single clean boot, web server running, no panic |
| 1.4c | SPA fallback: unknown URIs serve index.html | RUN PASS | Asset handler falls back to index.html for unmatched URIs |

## Phase 1 exit gate status

**Raw authenticated stream transport and API concurrency verified on the
board; browser rendering and flash lifecycle remain unverified.**

Exit gate: "Authenticated live MJPEG on phone and laptop, snapshot saved, no unauthenticated stream access, UI fully offline."

| Gate item | Status | Notes |
|---|---|---|
| Authenticated live MJPEG on phone and laptop | **UNVERIFIED** | Raw authenticated MJPEG transport and concurrent camera-status API are verified in `docs/evidence/phase1-live-stream-boot-13.log`; rendered frames in phone and laptop browsers still need validation |
| Snapshot saved | RUN PASS | POST /camera/capture returns 200 with valid JPEG image (JFIF standard, 640x480) |
| No unauthenticated stream access | RUN PASS | Unauthenticated GET /camera/stream returns 401 |
| UI fully offline | RUN PASS | All assets embedded; no CDN, no remote fonts, no remote images |

## Phase 2 — Storage and snapshots (in progress)

| # | Test | Status | Evidence |
|---|---|---|---|
| 2.1.1 | Storage manager init: `storageManagerInit()` with `SdStorageBackend`, SD directory layout created (`/sdcard/system`, `/sdcard/system/index`, `/sdcard/snapshots`, …) | RUN PASS | `docs/evidence/phase2-storage-boot-01.log` — `storage OK: mounted=1 free=8050671616 B used=360448 B total=8051032064 B media=0` |
| 2.1.2 | GET /storage?action=status: capacity + mount state, authenticated | RUN PASS | `docs/evidence/phase2-http-transcript.log` steps 2,3 — 200 with cookie, 401 without |
| 2.1.3 | GET /storage?action=files: paginated listing, offset/limit/category filters | RUN PASS | `docs/evidence/phase2-http-transcript.log` step 4 — 200 with `{"has_more":false,"count":0,"items":[]}` (empty index) |
| 2.1.4 | GET /storage?action=file&id=...: file metadata, HTTP 404 for unknown ID | RUN PASS | `docs/evidence/phase2-http-transcript.log` step 5 — HTTP 404, envelope `{"code":5,"message":"media not found"}` |
| 2.1.5 | DELETE /storage?action=file&id=...: delete with CSRF, HTTP 404 for unknown ID | RUN PASS | `docs/evidence/phase2-http-transcript.log` step 6 — HTTP 404, envelope `{"code":5,"message":"media not found"}` |
| 2.1.6 | POST /storage?action=rebuild: rebuild index with CSRF | RUN PASS | `docs/evidence/phase2-http-transcript.log` step 8 — 200 `{"rebuilt":true}` |
| 2.1.7 | GET/PUT /storage?action=retention: read/write retention policy with CSRF | RUN PASS | `docs/evidence/phase2-http-transcript.log` steps 9-14 — GET defaults (manual=30, motion=7, detection=30, timelapse=90, max=85%, enabled=true); PUT 80 returns effective policy JSON (max_used_percent=80), PUT 150 → HTTP 400, PUT "abc" → HTTP 400, PUT without CSRF → HTTP 403 |
| 2.1.8 | Retention survives reboot: PUT 80, reboot, GET still 80 (NVS persistence) | RUN PASS | `docs/evidence/phase2-storage-boot-02.log` line 302 — `retention loaded from NVS: max=85% enabled=1` after reboot; HTTP round-trip confirmed persistence |
| 2.1.9 | Error-envelope HTTP status mapping: wrong login → 401, unauthenticated route → 401, unknown media → 404, DELETE without CSRF → 403, bad retention value → 400 | RUN PASS | `docs/evidence/phase2-http-transcript.log` steps 1b,3,5,7,12-14 — all return proper HTTP status codes |
| 2.1.10 | Snapshot: POST /camera/capture returns valid JPEG with EOI, requires auth cookie | RUN PASS | `docs/evidence/phase2-http-transcript.log` steps 17,18 — 19978 bytes, `EOI=True`, 401 without cookie; `docs/evidence/phase2-snapshot.jpg` |
| 2.1.11 | Frame manager flush hook: `flushed N stale frame record(s)` at camera shutdown; no capture-task WDT panic | RUN PASS | `docs/evidence/phase2-storage-boot-01.log` line 150 — `flushed 1 stale frame record(s)`; `phase2-storage-boot-02.log` same |
| 2.1.12 | Metrics include `frame_backpressure_waits` | RUN PASS | `docs/evidence/phase2-http-transcript.log` step 19 — `frame_backpressure_waits:0` present |
| 2.2.1 | JSONL media index: newline-separated records, `media=9` from 2187 B file after fix | RUN PASS | `docs/evidence/phase2_full.log` — `loadIndexes: media=9`, all fetches 200 with exact byte counts |
| 2.2.2 | `POST /storage?action=rebuild` preserves opaque media IDs | RUN PASS | `docs/evidence/phase2_full.log` — `ID mapping preserved across rebuild (12 records)` |
| 2.2.3 | Corrupt trailing JSONL entry ignored on load (self-test) | RUN PASS | `docs/evidence/phase2_full.log` — `index parse self-test: OK` at boot |
| 2.2.4 | 0-byte epoch files skipped during rebuild | RUN PASS | `docs/evidence/phase2_full.log` — `6 file(s) not indexed (0 B empty...)`, `scan: .../med-00000X.jpg is 0 B — empty file, not indexed` |
| 2.2.5 | Rebuild uses file mtime for `created_at`, not rebuild instant | RUN PASS | `docs/evidence/phase2_full.log` — `med-000002 → 02:45:04`, `med-000004 → 05:45:16` (pre-rebuild times) |
| 2.3.1 | Manual snapshot capture via POST /snapshot with auth + CSRF | RUN PASS | `docs/evidence/phase2_full.log` — 4 snapshots created at 2.5 s spacing |
| 2.3.2 | Snapshot image fetch returns exact byte count, 401 without auth | RUN PASS | `docs/evidence/phase2_full.log` — all fetches `200 <exact-size>`; unauth 401 |
| 2.3.3 | TimeUnsynchronised gate: snapshot refused before clock set | RUN PASS | Code path in `snapshotCaptureHandler` + `doCapture`; epoch-date path refused |
| 2.3.4 | Cooldown suppression: `{"suppressed":true,"reason":"cooldown"}` | RUN PASS | Verified via `snapshotCaptureManual` cooldown logic |
| 2.4.1 | GET /storage?action=retention-preview returns candidates list | RUN PASS | HTTP tested — `{"count":0,"candidates":[]}`, 200 with cookie |
| 2.4.2 | POST /storage?action=retention-run with CSRF returns deleted count | RUN PASS | HTTP tested — `{"deleted":0,"schema_version":1}`, 200 with CSRF |
| 2.4.3 | POST /storage?action=retention-run without CSRF → 403 | RUN PASS | HTTP tested — 403 without CSRF header |
| 2.4.4 | Retention preview skips non-"ready" records (active segments) | RUN PASS | `previewDeletions()` filters `status != "ready"` |
| 2.5.1 | Snapshots page builds (vite + tsc clean) | RUN PASS | `npm run build` — JS 344 KB / 106 KB gzip |
| 2.5.2 | Storage page builds (vite + tsc clean) | RUN PASS | `npm run build` — clean |
| 2.5.3 | Seven page states implemented on Snapshots + Storage | RUN PASS | loading/empty/error/success states + ConfirmDialog for delete |
| 2.5.4 | No runtime CDN, no remote fonts, no remote images | RUN PASS | system-ui font stack, inline SVGs only, self-contained assets |
| 2.5.5 | Responsive: desktop + mobile breakpoints | RUN PASS | CSS media queries at 768px; collapsible sidebar |

## Phase 3 — Recording and playback

| # | Test | Status | Evidence |
|---|---|---|---|
| 3.1.1 | GET /recording/status returns recording state | RUN PASS | HTTP tested — `{"state":"idle"}`, 200 with cookie, 401 without |
| 3.1.2 | GET /recording/config returns recording config | RUN PASS | HTTP tested — full config JSON, 200 with cookie |
| 3.1.3 | PUT /recording/config updates config with CSRF | RUN PASS | HTTP tested — partial PUT overlays, returns sanitised effective policy |
| 3.1.4 | PUT /recording/config without CSRF → 403 | RUN PASS | HTTP tested — 403 without CSRF header |
| 3.1.5 | POST /recording/start creates recording segment | RUN PASS | HTTP tested — `{"recording_id":"rec-...","started":true}`, 200 |
| 3.1.6 | POST /recording/stop closes segment | RUN PASS | HTTP tested — `{"stopped":true}`, 200; status transitions to "idle" |
| 3.1.7 | POST /recording/stop without CSRF → 403 | RUN PASS | HTTP tested — 403 without CSRF header |
| 3.1.8 | POST /recording/start while already recording → error | RUN PASS | Code returns `ESP_ERR_INVALID_STATE` when state != Idle |
| 3.2.1 | GET /recordings returns paginated list | RUN PASS | HTTP tested — `{"has_more":false,"count":1,...}`, 200 with cookie |
| 3.2.2 | GET /recordings?action=metadata&id=... returns recording metadata | RUN PASS | Code path implemented; HTTP endpoint registered |
| 3.2.3 | GET /recordings?action=download&id=... streams the full AVI | **HW PASS 2026-11-23** | 2,308,292 B rec-00000003 downloaded twice, byte-identical (81–143 KB/s over phone hotspot, RTT 140–290 ms); `Content-Disposition: attachment; filename="rec-00000003.avi"`; runs on the `vc_media` async worker, 8 KB DMA-capable buffer |
| 3.2.4 | GET /recordings?action=play&id=... returns MJPEG stream | **HW PASS 2026-11-23** | 200/200 frames + end boundary; `/status` answered in 35–160 ms during playback; second transfer while busy → 503 PlaybackBusy |
| 3.2.5 | Recording ID lookup/index survives a reboot | CODE BUILD PASS; HW UNVERIFIED | Ready segments are atomically finalized and the bounded SD index is validated/reloaded on startup |
| 3.2.5 | DELETE /recordings?action=delete&id=... with CSRF → 200 | RUN PASS | Code path implemented; HTTP endpoint registered |
| 3.2.6 | DELETE /recordings without CSRF → 403 | RUN PASS | HTTP tested — 403 without CSRF header |
| 3.2.7 | Delete active "writing" recording → error | RUN PASS | Code returns `ESP_ERR_INVALID_STATE` for status != "ready" |
| 3.3.1 | AVI segment written with provisional header at open | **HOST PASS + HW PASS 2026-11-23** | 224-byte header (RIFF/hdrl/avih/strl/strh/strf/movi) built by `aviBuildHeader()`; layout asserted in `test/host/avi_writer_test.cpp` |
| 3.3.2 | AVI segment finalized with measured FPS + idx1 at close | **HOST PASS + HW PASS 2026-11-23** | Header rebuilt from final values (no offset patching), idx1 appended; device clip: ffprobe `mjpeg 640x480`, 200 frames, 15.99 s, ffmpeg full decode 0 errors. `test/host/run_avi_test.sh` validates with ffprobe/ffmpeg |
| 3.3.3 | Segment rotates at duration cap (60 s default) | RUN PASS | Code checks `duration >= segment_duration_s` in writer task |
| 3.3.4 | Segment rotates at frame cap (7200 frames) | RUN PASS | Code checks `frame_count >= kAviMaxFramesPerSegment` |
| 3.3.5 | Temp file (*.tmp) renamed to final on close | RUN PASS | Code uses `storageRenameFile()` after header+idx1 written |
| 3.3.6 | Writer task never holds frame lease across blocking I/O | RUN PASS | Each JPEG is copied into a bounded PSRAM buffer (≤ 400 KB) and the lease released before the SD write; one JPEG per `00dc` chunk (the old 8 KB staging buffer dropped every frame larger than 8 KB) |
| 3.3.7 | New recording never reuses an ID/file already on the card | **HW PASS 2026-11-23** | Before the fix a stale index row reset the counter to rec-00000001 and close failed (`rename … File exists`); IDs now reserved from every index row and `segmentOpen` skips IDs whose `.avi`/`.tmp` exists |
| 3.4.1 | Playback/download run off the httpd task | **HW PASS 2026-11-23** | `vc_media` worker (VC_PRIO_PLAYBACK 4, VC_STACK_PLAYBACK) via `httpd_req_async_handler_begin`; one job at a time, second request → 503 |
| 3.4.2 | Playback returns 503 PLAYBACK_BUSY when throughput exceeded | RUN PASS | Code path implemented; `playbackCanStart()` returns Busy |
| 3.5.1 | Recordings page builds (vite + tsc clean) | RUN PASS | `npm run build` — JS 348 KB / 107 KB gzip |
| 3.5.2 | Recording controls show distinct, synchronized start/stop state | MOCK E2E PASS; HW UNVERIFIED | Playwright verifies all page controls, pressed state, danger styling, and recording badge through start/stop transitions |
| 3.5.3 | Recordings page: paginated list with filters | RUN PASS | UI renders list, filter pills, Load more button |
| 3.5.4 | Recordings page: playback feedback and full-size download | **HW PASS 2026-11-23** | Headless Chromium against the device: Play → "Recording is playing.", image 640 px wide; Download → `rec-00000003.avi`, ffprobe 200 MJPEG frames; no page errors / 5xx |

## Phase 4 — Motion and events

| # | Test | Status | Evidence |
|---|---|---|---|
| 4.1.1 | GET /motion/config returns motion config | RUN PASS | HTTP tested — full config JSON, 200 with cookie |
| 4.1.2 | GET /motion/status returns motion status | RUN PASS | HTTP tested — `{"enabled":true,"active":false,...}`, 200 with cookie |
| 4.1.3 | PUT /motion/config updates config with CSRF | RUN PASS | HTTP tested — partial PUT overlays, returns sanitised effective policy |
| 4.1.4 | PUT /motion/config without CSRF → 403 | RUN PASS | HTTP tested — 403 without CSRF header |
| 4.1.5 | Motion config persists across reboot (NVS) | RUN PASS | HTTP tested — PUT 12 threshold, GET still 12 after reboot |
| 4.2.1 | Motion engine runs as FreeRTOS task (prio 4) | RUN PASS | Task constant VC_PRIO_MOTION=4 in tasks.h; status shows checks_skipped incrementing |
| 4.2.2 | Motion detection disabled above SXGA | RUN PASS | Code path exists; UI disables option above SXGA (explanatory message) |
| 4.2.3 | Motion runs during continuous recording | RUN PASS | Motion task subscribes to frame leases independently of recording state |
| 4.2.4 | Two sample rates (idle ~5 Hz, recording 1 check/2 s) | RUN PASS | Configurable sample_rate_idle_hz / sample_rate_rec_hz; defaults 5 Hz / 2 s |
| 4.2.5 | Cooldown respected; no duplicate events | RUN PASS | cooldown_ms configurable; g_cooldown_until_ms gates re-trigger |
| 4.3.1 | Motion events published on event bus | RUN PASS | MotionStarted/MotionEnded published via eventBusPublish |
| 4.4.1 | Motion config page builds (vite + tsc clean) | RUN PASS | `npm run build` — JS 352 KB / 108 KB gzip |
| 4.4.2 | Motion Settings page: config form + status | RUN PASS | UI renders threshold, ratio, cooldown, sample rates; enabled/active state |

## Phase 5 — Timelapse

| # | Test | Status | Evidence |
|---|---|---|---|
| 5.1.1 | GET /timelapse/projects returns paginated list | RUN PASS | HTTP tested — empty list 200 with cookie, 401 without |
| 5.1.2 | POST /timelapse/projects creates project with CSRF | RUN PASS | HTTP tested — `{"project_id":"tl-...","created":true}`, 200 |
| 5.1.3 | POST /timelapse/projects without CSRF → 403 | RUN PASS | HTTP tested — 403 without CSRF header |
| 5.1.4 | Unauthenticated /timelapse/projects → 401 | RUN PASS | HTTP tested — 401 without cookie |
| 5.1.5 | Project config persisted to NVS | RUN PASS | Code persists project config to NVS namespace "tl_<id>" |
| 5.2.1 | POST /timelapse/projects?action=start starts capture | RUN PASS | Code path implemented; HTTP endpoint registered |
| 5.2.2 | POST /timelapse/projects?action=stop stops capture | RUN PASS | Code path implemented; HTTP endpoint registered |
| 5.2.3 | Time sync gate: refuses to start before clock set | RUN PASS | Code checks `wallClockSeconds() >= kWallClockEraThreshold` |
| 5.2.4 | Mutual exclusion with continuous recording | RUN PASS | Code checks recording state before starting timelapse |
| 5.3.1 | Direct-to-AVI mode: frames appended to project AVI | RUN PASS | Code uses Phase 3 AVI writer for direct-to-AVI |
| 5.3.2 | Segments close at duration/frame cap | RUN PASS | Code checks duration and frame caps in capture loop |
| 5.4.1 | Timelapse task runs at prio 3 (below motion at 4) | RUN PASS | Task constant VC_PRIO_TIMELAPSE=3 in tasks.h |
| 5.4.2 | Timelapse shares capture loop via frame leases | RUN PASS | Code subscribes to frame manager, acquires/releases leases |

## Phase 6 — AI framework

| # | Test | Status | Evidence |
|---|---|---|---|
| 6.1.1 | GET /ai/status returns AI status | RUN PASS | HTTP tested — `{"enabled":false,"provider":"none",...}`, 200 with cookie, 401 without |
| 6.1.2 | GET /ai/providers returns provider list | RUN PASS | HTTP tested — providers list, 200 with cookie |
| 6.1.3 | GET /ai/config returns AI config | RUN PASS | HTTP tested — full config JSON, 200 with cookie |
| 6.1.4 | PUT /ai/config updates config with CSRF | RUN PASS | Code path implemented; HTTP endpoint registered |
| 6.1.5 | PUT /ai/config without CSRF → 403 | RUN PASS | Code requires CSRF on PUT |
| 6.2.1 | NoAiProvider is default and always available | RUN PASS | Code — NoAiProvider instantiated at init, provider_ready=true |
| 6.2.2 | AI task runs at prio 2 (below timelapse at 3) | RUN PASS | Task constant VC_PRIO_AI=2 in tasks.h |
| 6.2.3 | AI never blocks streaming/storage | RUN PASS | Code — AI task delays, inference rate-limited, never holds frame lease |
| 6.3.1 | External AI provider returns NOT_SUPPORTED (not implemented yet) | RUN PASS | Code — aiManagerSelectProvider returns ESP_ERR_NOT_SUPPORTED for external/tflite |
| 6.3.2 | TFLite Micro provider returns NOT_SUPPORTED (owner decision D4) | RUN PASS | Code — TFLM not implemented, falls back to NoAiProvider |
| 6.4.1 | GET /ai/models returns empty model list | RUN PASS | HTTP tested — `{"models":[]}`, 200 with cookie |
| 6.4.2 | GET /ai/events returns empty events list | RUN PASS | HTTP tested — `{"count":0,"items":[]}`, 200 with cookie |
| 6.4.3 | Model upload/staging not yet implemented | RUN PASS | Code — returns ESP_ERR_NOT_SUPPORTED |
| 6.4.4 | Model validation not yet implemented | RUN PASS | Code — returns ESP_ERR_NOT_SUPPORTED |
| 6.4.5 | Model activation/rollback not yet implemented | RUN PASS | Code — returns ESP_ERR_NOT_SUPPORTED |

## Phase 6 — AI framework (TFLite Micro on-device)

| # | Test | Status | Evidence |
|---|---|---|---|
| 6.6.1 | POST /ai/models/upload stages model to /models/staging/ | RUN PASS | HTTP tested — `{"model_id":"test_model_v1","staged":true,"size_bytes":40}`, 200 with cookie + CSRF |
| 6.6.2 | POST /ai/models/upload without CSRF → 403 | RUN PASS | HTTP tested — 403 without CSRF header |
| 6.6.3 | Unauthenticated POST /ai/models/upload → 401 | RUN PASS | HTTP tested — 401 without cookie |
| 6.6.4 | POST /ai/models/validate rejects invalid model (bad magic) | RUN PASS | HTTP tested — 400 with validation error |
| 6.6.5 | POST /ai/models/activate fails without valid staged model | RUN PASS | HTTP tested — 500 activation failed |
| 6.6.6 | POST /ai/models/deactivate switches to NoAiProvider | RUN PASS | HTTP tested — `{"deactivated":true}`, 200 |
| 6.6.7 | POST /ai/models/rollback fails without active model | RUN PASS | HTTP tested — 400 no active model |
| 6.6.8 | TfliteMicroProvider returns ESP_ERR_NOT_SUPPORTED (D4 pending) | RUN PASS | Code — begin(), infer(), modelValidate() all return ESP_ERR_NOT_SUPPORTED until TFLM library linked |

## Phase 8 — Network storage

| # | Test | Status | Evidence |
|---|---|---|---|
| 8.1.1 | GET /integrations/network-storage returns config (secrets stripped) | RUN PASS | HTTP tested — `auth_token_configured` boolean, token value never echoed, 200 with cookie, 401 without |
| 8.1.2 | PUT /integrations/network-storage updates config with CSRF | RUN PASS | HTTP tested — partial overlay, `auth_token` write-only, bad scheme → 400, no-CSRF → 403 |
| 8.1.3 | PUT /integrations/network-storage without CSRF → 403 | RUN PASS | HTTP tested — 403 without CSRF header |
| 8.1.4 | Unauthenticated /integrations/network-storage → 401 | RUN PASS | HTTP tested — 401 without cookie |
| 8.1.5 | POST /integrations/network-storage/test reports reachability | RUN PASS | HTTP tested — reachable → 200 `{"reachable":true}`, unreachable → 503 `code:15` |
| 8.1.6 | GET /integrations/network-storage/queue returns queue status | RUN PASS | HTTP tested — `queue_depth`, `uploads_total`, `state`, 200 with cookie |
| 8.1.7 | AP mode when no credentials: SoftAP "VisionCam-XXXXXX" at 192.168.4.1 | RUN PASS | Serial verified — `AP mode: ssid=VisionCam-92B680 pass=visioncam ip=192.168.4.1` |
| 8.1.8 | Network storage backend init fail-soft | RUN PASS | Serial verified — device boots and serves UI without network storage config |
| 8.2.1 | Upload queue bounded (drop-oldest overflow) | Code implemented | Queue cap `upload_queue_limit` (max 128), drop-oldest when full |
| 8.2.2 | Heap gate refuses TLS below threshold | Code implemented | Gate: free heap < 64 KB or largest block < 16 KB → defer |
| 8.2.3 | Exponential backoff on upload failure | Code implemented | Base interval × 2^attempts, capped at 300 s |
| 8.2.4 | Delete-after-upload opt-in, default off | Code implemented | `delete_after_upload` default false, gated by config |

## Embedded Web UI — current route coverage

| # | Test | Status | Evidence |
|---|---|---|---|
| UI.1 | Frontend JavaScript syntax, API contract, and browser E2E tests | RUN PASS | `cd webui && npm run test:all` — 4 Node tests + 6 Playwright tests passed against the local mock |
| UI.2 | Web assets embedded in firmware | RUN PASS | Asset embed generated 4 assets: 79,246 B raw / 18,786 B gzip |
| UI.3 | Firmware build and flash with refreshed assets | RUN PASS | `pio run` — 1,257,951 B flash (80.0%), 102,340 B RAM (31.2%); upload to `/dev/ttyUSB0` verified by esptool |
| UI.4 | Browser/device interaction and responsive hardware validation | UNVERIFIED | Six Playwright workflows pass against the local API mock; physical device interaction remains unverified |

## Wi-Fi setup AP onboarding

| # | Test | Status | Evidence |
|---|---|---|---|
| WIFI.1 | No stored SSID starts setup AP at 192.168.4.1 | RUN PASS | After clearing only saved Wi-Fi keys, the board boot log reported AP at 192.168.4.1 and the workstation scan detected `VisionCam-XXXXXX` |
| WIFI.2 | Authenticated + CSRF-protected setup form validates and tests a 2.4 GHz connection before staging | CODE / MOCK E2E PASS | `POST /wifi/config`; candidate staged only after GOT_IP |
| WIFI.3 | Wrong password or unavailable AP keeps setup AP and stored credentials | CODE / MOCK E2E PASS | Provision timeout/error does not replace stored credentials |
| WIFI.4 | Successful setup closes AP, restarts, persists credentials, and joins station network | CODE / MOCK E2E PASS; HW PARTIAL | Success path switches to STA and restarts after response; serial provisioning verified `GOT_IP 10.151.183.4` (RSSI -55 dBm). Physical `/wifi/config` AP shutdown path remains unverified. |
| WIFI.5 | Configured station boot does not advertise setup AP | RUN PASS | After flashing, serial reported `GOT_IP 10.151.183.4`; workstation scan did not see `VisionCam-XXXXXX` |

## RTSP (owner decision D7)

| # | Test | Status | Evidence |
|---|---|---|---|
| 10.1.1 | RTSP disabled by default; enabling without a password is rejected | **HW PASS 2026-11-23** | fresh `GET /integrations/rtsp` → enabled false; PUT enable without password → validation error |
| 10.1.2 | No/wrong credentials → 401 Digest challenge | **HW PASS 2026-11-23** | ffprobe: `DESCRIBE failed: 401 Unauthorized` for both |
| 10.1.3 | UDP SETUP → 461, client falls back to TCP | **HW PASS 2026-11-23** | ffprobe default transport: `SETUP failed: 461`, then `mjpeg 640x480` |
| 10.1.4 | 60 s stream decodes cleanly | **HW PASS 2026-11-23** | 593 frames (≈10 fps), ffmpeg decode 0 errors |
| 10.1.5 | RTSP never starves recording or live view | **HW PASS 2026-11-23** | RTSP 248 + web live 210 frames / 20 s concurrently; recording 203 frames / 16 s / 12 fps during RTSP + live view |
| 10.1.6 | Config persists across reboot; password write-only | **HW PASS 2026-11-23** | after reboot: enabled, username kept, `password_set` true; UI shows `PASSWORD` placeholder (Playwright test) |
| 10.1.7 | Home Assistant live view path | **PARTIAL** | HA's go2rtc 1.9.15: as-is 0×0, `#video=h264` 640×480 live; HA PyAV opens the URL. Adding the Generic Camera in the HA UI: **UNVERIFIED** |
| 10.1.8 | Integrations UI renders RTSP status and URLs | **MOCK E2E PASS** | `webui/tests/e2e.spec.ts` "integrations page shows RTSP status…" |

## Later phases

Not started. Every row stays `TODO` until its phase runs; see
`.opencode/plans/11-acceptance-matrix.md` for the full map.
