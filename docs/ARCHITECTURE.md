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
| core services (`core_init`, event bus, RTC ring log, resource monitor) | 1 | `coreInit()` runs first in `app_main`; typed bounded event queue (depth 16, drop-oldest); RTC ring log + crash-loop counter with health point at end of boot; heap thresholds 32 KB free / 16 KB largest internal |
| camera manager | 1 | R2 init, PID check, warm-up, PWDN retry; live frame-size/quality settings; SPEC 9.1 recovery ladder (5 sensor-reset rungs, then power-cycle + re-init; 2 s gate, 10-attempt cap, terminal `CameraFailed`) |
| frame manager | 1 | sole `esp_camera_fb_get`/`fb_return` owner (AGENTS rule 9); bounded pool (4 records), per-consumer queues (depth 2, drop-oldest), ~10 ms yield per frame |
| Wi-Fi manager | 1 | station mode; lwIP always init (web server needs tcpip); reconnect with exponential backoff + jitter; `Connected` only on GOT_IP |
| HTTP server + embedded UI | 1 | `esp_http_server` on port 80, lru_purge, 4 KB stack, 6 max_open_sockets, 16 max_uri_handlers; `{ok,data,error,request_id}` envelope; auth + CSRF on all routes; embedded gzipped web assets (React UI) served with Content-Encoding: gzip; SPA fallback to index.html |
| auth | 1 | single user (admin/admin default); salted iterated SHA-256 password hash; RAM-only CSPRNG sessions; HTTP-only SameSite cookies; CSRF double-submit; login throttling; session revocation on password change |
| storage manager | 2 | SD 1-bit mount, FATFS VFS, no auto-format |
| media index | 2 | versioned, paginated, opaque media IDs; JSONL + rebuildable cache; per-line validation; boot-time self-test |
| snapshot manager | 2 | manual capture on demand; fresh frame subscriber per capture; era guard (refuses 1970/01/01); cooldown; opaque media IDs |
| retention | 2 | policy read/write + preview + run; deletes only "ready" records; logs every deletion |
| recording manager | 3 | segment-based MJPEG AVI writer; provisional header at open; measured-FPS header at close; idx1 index; segment rotation at duration/frame cap |
| playback | 3 | server-side MJPEG replay from SD; 00dc chunk walking; idx1-based seek; lower priority than writer; 503 PLAYBACK_BUSY |
| motion engine | 4 | JPEG decode to 96x96 grayscale; SAD compare with previous frame; MotionStarted/MotionEnded events; event recording triggers |
| timelapse manager | 5 | scheduled JPEG capture per project; direct-to-AVI or frames+generate; project.json metadata; time-sync gate; mutual exclusion with recording |
| timelapse task | 5 | captures one frame per interval; appends to project AVI (direct-to-AVI); shares capture loop via frame leases |
| AI framework | 6 | external-first; camera never blocked |
| MQTT / Home Assistant | 7 | fail-soft, stable `visioncam/{device_id}` IDs |
| network storage | 8 | upload after segment close; local files preserved |
| hardening | 9 | 24 h mixed workload, degraded-mode drills |

## 4. Resource measurements (fill at the end of every phase)

Record **heap, largest free block, stack high-water marks and frame rates**.
Never estimate — paste real numbers or write `UNVERIFIED`.

| Phase | Free heap B | Min heap B | Largest block B | Stack HWM (worst task) | FPS measured | Date / build |
|---|---|---|---|---|---|---|
| 0 | 3,588,215 (after Wi-Fi, no capture loop) | 3,549,752 | 3,407,872 | UNVERIFIED | UNVERIFIED — needs the Phase 1 capture loop; the R1 bar it must match is now recorded below | D2 reflash 2026-11-15 (smoke figures below) |
| 1 | 4,429,511 (after boot banner) | 3,535,512 (after Wi-Fi step 14) | 3,014,656 | `main` 6,508 B / `vc_capture` unknown (not reported yet) | UNVERIFIED — capture loop FPS needs Phase 1 stream test | 2026-11-17 (phase1-web-boot-04.log) |
| 2 | 3,539,887 (after Phase 2 UI snapshots + rebuild traffic) | 2,955,416 | 47,104 | `httpd` 7,264 B peak / `vc_capture` 3,712 B / `main` 5,604 B | UNVERIFIED — FPS needs Phase 3 capture loop | 2026-11-19 (phase2_full.log) |
| 3 | 3,342,704 (after recording writer + playback tasks) | 2,955,416 | 47,104 | `httpd` 7,264 B / `vc_writer` UNVERIFIED / `vc_capture` 3,712 B | UNVERIFIED — recording FPS needs capture loop integration | 2026-11-19 (phase3 build) |
| 4 | 2,955,416 | 2,955,416 | 47,104 | `httpd` 7,264 B / `vc_motion` UNVERIFIED / `vc_capture` 3,712 B | UNVERIFIED — motion FPS needs capture loop | 2026-11-19 (phase4 build) |
| 5 | 2,955,416 | 2,955,416 | 47,104 | `httpd` 7,264 B / `vc_timelapse` UNVERIFIED / `vc_capture` 3,712 B | UNVERIFIED — timelapse FPS needs capture loop | 2026-11-19 (phase5 build) |

**Smoke-run measurements (2026-11-17, board attached, evidence in
`docs/evidence/`):**

| Point in boot | Free heap B | Min heap B | Largest block B |
|---|---|---|---|
| after boot banner | 4,429,511 | 4,397,432 | 4,128,768 |
| after camera init | ~3,620,000 | ~3,580,000 | 3,407,872 |
| after SD mount | ~3,613,000 | ~3,580,000 | 3,407,872 |
| after Wi-Fi step 14 | 3,573,983 | 3,535,512 | 3,014,656 |
| after web server start | ~3,560,000 | ~3,535,000 | ~3,000,000 |

PSRAM: 8,388,608 B fitted, 4,192,024 B in the free-heap pool at boot, **4 MB
mapped** by IDF (see `docs/HARDWARE.md` §2). Largest PSRAM block 4,128,768 B.

Flash usage (Phase 1 build, `pio run`): 1,248,540 B / 1,572,864 B = **79.4 %**
of the `ota_0` app slot. Remaining headroom: ~324 KB — the Phase 1 web
bundle adds ~107 KB gzipped (350 KB raw). RAM: 56,224 B / 327,680 B = **17.2 %**.

**R1 baseline (D2, measured on the owner's board 2026-11-15 — the acceptance
bar; full table with heap figures in `docs/HARDWARE.md` §4):**

| Reference point (quality 12) | Recording FPS | SD write | Recorder busy |
|---|---|---|---|
| QVGA 320×240 | 30.8 (R1 target 30) | 0.51 MB/s | 40 % |
| VGA 640×480 | 20.6 (target 20) | 0.50 MB/s | 67 % |
| SVGA 800×600 | 20.4 (target 20) | 0.49 MB/s | **100 %** |
| XGA 1024×768 | 5.3 (target 5) | 0.49 MB/s | 52 % |
| SXGA 1280×1024 | 6.2 (target 5) | 0.49 MB/s | 98 % |

R1's recording FPS is essentially the requested FPS up to SVGA; the card is
saturated at SVGA/SXGA, so those two sizes are **write-limited**, not
capture-limited. Phase 3's parity test (§11.10, tolerance **10 %** — D3) must
compare like for like and, above SVGA, compare against the same bottleneck.

**Storage capacity reality check (affects storage manager, retention and
Phase 9) — RESOLVED 2026-11-16:** the owner's card reported **CSD
8,053,063,680 B** (CID product name `SD16G`) but presented a **5,216,256 B
FAT12 volume with 1,349,632 B free** — both FATFS implementations measured
identically, so the formatted volume did not cover the card
(`docs/HARDWARE.md` §4.2). The cause turned out to be a **bootable Linux
Mint image** (EFI + ext4; owner identified it 2026-11-16), and the owner
authorized a rewrite: `tools/sd_format/` reformatted the card to **MBR +
FAT32, 8,051,032,064 B usable (99 % of CSD)**, re-verified by VisionCam
boot 12 with no coverage warning. Consequences that remain in force:

- size retention against the **mounted volume**, never against the CSD;
- the storage manager's free-space policy must be **bounded** and must never
  delete anything that is not a VisionCam recording (R1's unbounded
  oldest-folder loop is rejected — `docs/REFERENCE_ANALYSIS.md`);
- VisionCam never auto-formats (SPEC §2.3) — the reformat was a
  human-gated one-shot tool, not firmware behaviour;
- SD write throughput measured on this card in 1-bit mode is ≈0.5 MB/s
  (D2 run), which is the number the recorder's buffering must be sized
  against; the 24-hour Phase 9 numbers now have a card with room.

Build size (verified 2026-11-15):

| Artifact | Value |
|---|---|
| RAM used, smoke build **with Wi-Fi** | 50,584 B / 327,680 B (15.4 %) |
| Flash used, smoke build **with Wi-Fi** | 1,041,544 B / 1,572,864 B = **66.2 %** of the `ota_0` app slot |
| Flash used, same build + D2 FAT-volume diagnostic | 1,042,324 B / 1,572,864 B = **66.3 %** (+780 B, RAM unchanged) |
| RAM used, smoke build without radio (earlier) | 26,840 B / 327,680 B (8.2 %) |
| Flash used, smoke build without radio (earlier) | 387,356 B = 24.6 % — `firmware.bin` 387,792 B |

The radio (Wi-Fi + mbedTLS + supplicant) costs **≈654 KB flash / ≈24 KB RAM**
— the single largest flash consumer so far; keep this in mind for the
Phase 6/7 budget.

Before `board_build.partitions` was set, the same binary read as 36.9 % —
PlatformIO was measuring against the framework's default 1 MB `factory`
partition instead of our 1.5 MB `ota_0`. Always check the denominator.
See `docs/adr/0001-toolchain.md` § "Partition table".

| Partition | Offset | Size |
|---|---|---|
| `nvs` | `0x009000` | 92 KB |
| `ota_0` (app slot A) | `0x020000` | 1.5 MB |
| `ota_1` (app slot B) | `0x1a0000` | 1.5 MB |
| `otadata` | `0x320000` | 8 KB |
| `phy_init` | `0x322000` | 4 KB |
| `storage` (models / UI fallback) | `0x323000` | 884 KB |

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

- ~~Partition table (two-slot OTA vs single app)~~ — **decided** (Phase 0,
  deliverable 5): two 1.5 MB OTA slots + 884 KB `storage`. Rationale in
  `partitions.csv` header and `docs/adr/0001-toolchain.md`. Kept two slots
  because the table cannot be changed later without a full erase; no OTA
  code exists yet (SPEC §5.4).
- Whether the web assets live in the app partition or in the 884 KB
  `storage` partition — still open, depends on the built web bundle size.
  The `storage` partition is reserved for exactly this plus model support,
  so either answer is accommodated without re-flashing the table.
