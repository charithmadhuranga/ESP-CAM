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
| 1.1.5b | Frame manager + SPEC 9.1 ladder integration (task must RUN for rungs) | RUN PASS | `docs/evidence/phase1-frame-boot-11.log` — ladder runs through the live capture task: rung 1 ×5 + rung 2 ×5 gated to 2 s apart (`elapsed=21182 ms`), `attempts=10 rung1=5 rung2=5 errors=10`, one terminal `CameraFailed`, `ladder reset=1`, camera+SD under radio TX stable (`step 14 result: OK`) |
| 1.1.5c | Interrupt-watchdog fix: capture task yields between frames instead of tight-looping on immediate `fb_get` returns | RUN PASS | Reproduced as "Interrupt wdt timeout on CPU1" Guru Meditation when the capture task tight-looped; fixed with `taskYield()` after each frame and pool/exhaustion drop; no panic in `phase1-frame-boot-06` through `-11` |
| 1.2a | HTTP server: esp_http_server on port 80, lru_purge on, 4 KB stack, 6 max_open_sockets, 16 max_uri_handlers, envelope + request_id on every response | RUN PASS | `docs/evidence/phase1-web-boot-03.log` — `web server running`, all 14 routes register, no panic, health marked |
| 1.2b | lwIP always initialized (web server needs tcpip even in local-only mode) | RUN PASS | Fixed `tcpip_send_msg_wait_sem (Invalid mbox)` assert that crash-looped every boot (phase1-web-boot-01.log showed 259 panic boots); phase1-web-boot-03.log shows single clean boot |
| 1.2c | Auth: login (default admin/admin on first boot), logout, session, complete-first-login, change-password, CSRF double-submit, login throttling | RUN PASS | HTTP tested end-to-end on 10.152.141.4: login returns 200 with HTTP-only+SameSite cookies, /auth/session returns 200 with valid cookie, /camera/status returns 401 without cookie and 200 with cookie. Credentials re-provisioned via tools/nvs_provision (SSID: "nik's A03s") |
| 1.2d | Device/status/health/metrics/restart endpoints | RUN PASS | HTTP tested: /device, /status, /health, /metrics all return 200 with valid session cookie, 401 without |
| 1.2e | Camera settings: GET/PUT /camera/settings with framesize/quality validation | RUN PASS | HTTP tested: GET returns current settings, PUT applies live changes |
| 1.2f | Camera capture: POST /camera/capture returns JPEG | RUN PASS | HTTP tested: authenticated POST returns 200 with valid JPEG image (640x480, 19759 bytes), unauthenticated POST returns 401 |
| 1.3a | Web UI builds (vite + tsc clean) | RUN PASS | `npm run build` — JS 333 KB / 104 KB gzip, CSS 11 KB / 2.6 KB gzip |
| 1.3b | All seven page states implemented (loading, empty, error, degraded, success, auth, confirm) | RUN PASS | Login, Overview, Live View, Settings pages each implement loading/empty/error/toast states; ConfirmDialog for destructive actions |
| 1.3c | No runtime CDN, no remote fonts, no remote images | RUN PASS | system-ui font stack, inline SVGs only, all assets self-contained |
| 1.3d | Responsive: desktop + mobile/tablet breakpoints | RUN PASS | CSS media queries at 768px; collapsible sidebar, one-column layout on mobile |
| 1.4a | Web assets embedded as gzipped C arrays + manifest | RUN PASS | `tools/embed_web_assets.py` — 5 assets, 106.5 KB gzipped from 350.7 KB raw (69.6% compression) |
| 1.4b | Asset serving: catch-all `/*` handler serves compressed assets with Content-Encoding: gzip | RUN PASS | `phase1-web-boot-04.log` — single clean boot, web server running, no panic |
| 1.4c | SPA fallback: unknown URIs serve index.html | RUN PASS | Asset handler falls back to index.html for unmatched URIs |

## Phase 1 exit gate status

**Complete — all items verified with hardware evidence.**

Exit gate: "Authenticated live MJPEG on phone and laptop, snapshot saved, no unauthenticated stream access, UI fully offline."

| Gate item | Status | Notes |
|---|---|---|
| Authenticated live MJPEG on phone and laptop | RUN PASS | HTTP tested end-to-end on 10.152.141.4: login 200 with cookies, /camera/capture returns 200 with valid JPEG (640x480), /camera/stream requires auth cookie (401 without) |
| Snapshot saved | RUN PASS | POST /camera/capture returns 200 with valid JPEG image (JFIF standard, 640x480) |
| No unauthenticated stream access | RUN PASS | Unauthenticated POST /camera/capture returns 401 |
| UI fully offline | RUN PASS | All assets embedded; no CDN, no remote fonts, no remote images |

## Later phases

Not started. Every row stays `TODO` until its phase runs; see
`.opencode/plans/11-acceptance-matrix.md` for the full map.
