# Reference analysis — adopt / adapt / defer / reject

Companion to `SPEC.md` §1A. One entry per subsystem, written **after** reading
the pinned reference source and **in my own words** — never copied from R1
(AGENTS rules 31–36).

## Licence policy — owner decision D1 (recorded here as the exit gate requires)

**Option A — clean-room, chosen 2026-11-15.**

- R1 (`s60sc/ESP32-CAM_MJPEG2SD`, **AGPL-3.0**, `APP_VER 10.10.1`,
  commit `abe84d89fc74367c21b644cb5101c9b89fd4bfb7`) is a **behavioural
  reference only**. Ideas, architecture, measured numbers and failure lessons
  may be reused. Source may **not** be copied, vendored, or mechanically
  translated — specifically the 310-byte AVI header byte table and the
  `frameSizeData` table must be written fresh from the format spec, not lifted.
  R1 is never linked into a release build.
- R2 (`espressif/esp32-camera`, **Apache-2.0**, 2.1.9,
  commit `b3f7fe802f41f17fd7a104d517c53f1f62c8414d`) is a normal pinned
  dependency under either licence option; its licence notice ships in the
  release.
- Pinned checkouts and how to re-verify them: `reference/pins.txt`,
  `reference/fetch_references.sh --verify`.

If the owner ever selects Option B, this file and every entry below must be
revisited before any R1 code is reused.

---

## Board profile and pins — 2026-11-15

- **Studied:** R1 `camera_pins.h:187-206` @ abe84d8, R2 `driver/include/esp_camera.h` @ b3f7fe8
- **What it does / limits / failure modes:** The AI-Thinker camera is wired
  to a fixed set of GPIOs; several are input-only (34–39) and cannot be
  re-tasked. GPIO16 is reserved for PSRAM, GPIO1/3 are the UART, and GPIO
  0/2/12/15 are boot strapping pins — driving them wrongly at reset prevents
  boot (GPIO12 additionally selects flash voltage). GPIO4 is both the flash
  LED and SD D1.
- **Decision:** **adopt** the mapping as a compile-time board profile.
- **Reason:** Verified in three independent sources that agree exactly
  (SPEC §2.2, R1 `camera_pins.h`, owner-supplied board table). Nothing to
  reinterpret; the risk is duplication, so it lives only in
  `firmware/include/board_profile.h` (AGENTS rule 6).
- **Divergence from R1:** none in values. VisionCam adds the SD pin map,
  which SPEC §2.2 did not tabulate and which the owner supplied.
- **Evidence:** static cross-check complete; hardware confirmation
  `UNVERIFIED`.

## Storage / SD card interface — 2026-11-15

- **Studied:** R1 `utilsFS.cpp:28,72,80-84` @ abe84d8, R2 n/a
- **What it does / limits / failure modes:** R1 mounts the card in 1-bit mode
  **by default** (`static bool use1bitMode = true;`), because on the original
  ESP32 1-bit is nearly as fast as 4-bit and frees GPIO4 and GPIO12. Immediately
  after `SD_MMC.begin(...)` it forces GPIO4 low, guarded by
  `CAMERA_MODEL_AI_THINKER`, with the comment that the SD library still
  initialises pin 4 *even in 1-line mode* — otherwise the lamp lights. If the
  mount fails R1 logs and continues rather than formatting.
- **Decision:** **adapt** — same1-bit default and same GPIO4-low-after-mount
  behaviour, but re-proven on the **ESP-IDF** SDMMC driver rather than
  Arduino's `SD_MMC`, because R1's workaround targets a different library.
  Auto-format stays permanently disabled.
- **Reason:** AGENTS rules 7 and 8 make 1-bit + GPIO4 a hardware constraint;
  R1's evidence is behavioural, not licence-relevant code.
- **Divergence from R1:** R1 exposes `formatIfMountFailed` as a user
  parameter; VisionCam never enables it (SPEC §2.3).
- **Evidence:** source confirmed at `utilsFS.cpp:81-84`;
  hardware confirmation `UNVERIFIED` (Phase 0 task 0.3 step 13).

## SDMMC mount — two divergences from SPEC §2.3's snippet — 2026-11-15

SPEC §2.3 labels its example "Example starting point (verify against the
pinned ESP-IDF)". It has now been verified against the pinned **ESP-IDF
6.1.0**, and two places differ from the snippet as written.
`firmware/src/sd_bringup.cpp` implements both and points at this section
(task 0.4 step 18).

**1. Do not set `host.flags = SDMMC_HOST_FLAG_1BIT` (SPEC line 620).**

- **Redundant.** The bus width comes from `slot.width`, not from the flag:
  `sdmmc_fix_host_flags()` (`sdmmc_common.c:365-377`) reads the slot width and
  masks `host.flags` down to `SDMMC_HOST_FLAG_1BIT` when it is 1, then
  `sdmmc_init_host_bus_width()` (`sdmmc_common.c:222`) never requests 4-bit.
  `slot.width = 1` alone is what actually forces 1-bit (AGENTS rule 7).
- **Harmful.** Assigning that single flag **clears
  `SDMMC_HOST_FLAG_DEINIT_ARG`**, which `SDMMC_HOST_DEFAULT()` carries.
  `vfs_fat_sdmmc.c:447` branches on it when unmounting: with the flag,
  `deinit_p(slot)` tears down only this slot; without it, `host.deinit()`
  tears down the whole SDMMC peripheral. Keeping the default flags is the
  correct behaviour.

**2. `sdmmc_slot_config_t.clk / cmd / d0` are inert on ESP32.**

- ESP32 uses `SOC_SDMMC_USE_IOMUX`, not the GPIO matrix, so the driver sets
  `use_gpio_matrix = false` (`sd_host_sdmmc.c:1216`) and the pin fields never
  take effect — they only matter on targets that have a GPIO matrix. Setting
  them would be harmless but would imply control we do not have, so they are
  not set.
- The pins are fixed per slot: slot 1 = CLK 14, CMD 15, D0 2, D1 4, D2 12,
  D3 13 (`soc/sdmmc_pins.h:21-26`) — exactly the AI-Thinker SD pins — and
  slot 0 is the SPI flash (`sd_host_sdmmc.c:1231`), so being on slot 1 is not
  optional. `SDMMC_HOST_DEFAULT()` already resolves to slot 1
  (`sdmmc_default_configs.h:65`); `sd_bringup.cpp` sets `host.slot`
  explicitly anyway so a future IDF default change cannot silently move the
  bus onto the flash pins. SPEC's snippet relies on the default and does not
  set it.
- **Consequence for the board profile:** the SD pin constants in
  `board_profile.h` cannot be handed to the driver at all. They are
  compile-time documentation, enforced by `static_assert` against
  `SDMMC_SLOT1_IOMUX_PIN_*` so a wrong pin map fails the build instead of
  silently mounting nowhere.

- **Evidence:** IDF 6.1.0 source paths cited above; `sd_bringup.cpp`
  compiles. Mount behaviour on hardware `UNVERIFIED` (no board attached).

## Motion detection cadence — 2026-11-15

- **Studied:** R1 `mjpeg2sd.cpp:27-28,123-131,489` and
  `motionDetect.cpp:718,729,762` @ abe84d8
- **What it does / limits / failure modes:** Motion is not a timer — it is a
  **frame counter** inside the capture loop. `doMonitor()` returns true every
  `FPS / moveStartChecks` frames while idle and every `FPS * moveStopSecs`
  frames while capturing; defaults are 5 and 2, giving ~5 checks/s idle and
  one check per 2 s while recording, and the stop value doubles as the
  post-motion hold time. Detection aborts above SXGA
  (`if (fsizePtr > FRAMESIZE_SXGA) return false`), and the JPEG is decoded to
  a 96×96 (or 64×64 under 3 MB PSRAM) grayscale sample whose size comes from
  `frameData[].sampleRate`.
- **Decision:** **adopt** the behaviour and expose both rates as
  configuration; **adapt** the implementation to ESP-IDF timing.
- **Reason:** SPEC §4 and §9 require exactly this behaviour, and the pinned
  source confirms it.
- **Divergence from R1:** VisionCam keeps motion running during continuous
  recording so events are still marked (R1 disables it); SPEC §1A.9 also
  rejects R1's "refuse playback while capturing".
- **Evidence:** source confirmed; performance `UNVERIFIED`.

## Camera initialisation — 2026-11-15

- **Studied:** R1 `mjpeg2sd.cpp` (`initCam`, warm-up, retry) @ abe84d8, R2
  `esp_camera.h:90-91` @ b3f7fe8
- **What it does / limits / failure modes:** Init must confirm the sensor PID
  is OV2640, discard ≥3 warm-up frames before trusting the image, and on
  failure power-cycle the camera via PWDN (GPIO32) and retry rather than
  giving up. Frame buffers live in PSRAM; the maximum JPEG buffer is
  `width*height/5`. `CAMERA_GRAB_LATEST` vs `CAMERA_GRAB_WHEN_EMPTY` decides
  whether a slow consumer sees stale frames or waits.
- **Decision:** **adopt** the sequence and limits (SPEC §4.1.1 already
  specifies it); **dependency adopt** R2 itself.
- **Reason:** R2 is Apache-2.0 and is the runtime driver — never a
  reimplementation target (SPEC §1A.3).
- **Divergence from R1:** none on init order.
- **Evidence:** R2 commit matches SPEC's recorded pin; driver smoke test
  `UNVERIFIED` (no board attached).

## Camera init / retry — pinned R2 facts vs its own documentation — 2026-11-15

- **Studied:** pinned esp32-camera 2.1.9 in `managed_components/` —
  `driver/include/esp_camera.h:187-188,203`,
  `driver/esp_camera.c:177-178,316-320,374-376,379-391`,
  `driver/include/sensor.h:21`, `driver/cam_hal.c:619`.
- **What was found:**
  - `esp_camera_init()`'s doc comment still says the module "can only be
    called once and there is no way to de-initialize"
    (`esp_camera.h:187-188`). **This is stale:** `esp_camera_deinit()` is
    declared (`esp_camera.h:203`), implemented (`esp_camera.c:379`) and nulls
    `s_state` (`esp_camera.c:387`).
  - Every *failure* path inside `esp_camera_init` reaches its `fail:` label,
    which already calls `esp_camera_deinit()` (`esp_camera.c:374-376`) — so
    retrying after a failed init works without extra help.
  - A *successful* init leaves `s_state` set, so a second `esp_camera_init()`
    returns `ESP_ERR_INVALID_STATE` from `camera_probe()`
    (`esp_camera.c:177-178`) unless deinit runs first. SPEC §4.1.1's PID
    check, warm-up discard and malformed-frame rejection all execute **after**
    a successful init, so those exit paths must deinit explicitly or a later
    bring-up can never recover. Taken at face value, R2's own doc comment
    would have forbidden the retry SPEC mandates.
  - `OV2640_PID` is **`0x26`**, not `0x2640` (`sensor.h:21`). SPEC asks for a
    PID check without quoting the constant — the trap is recorded here.
  - R2's default JPEG receive buffer is `width*height/5` (`cam_hal.c:619`) —
    **confirms** SPEC's "Proven design fact" instead of diverging from it.
- **Decision:** **adopt** the retry sequence with an explicit
  `cameraShutdown()` (= `esp_camera_deinit()`) before any re-init and on every
  post-init exit path (`camera_bringup.cpp`).
- **Evidence:** source lines cited above; on-device retry and PWDN recovery
  `UNVERIFIED` (no board attached).

---

## §1A.5 matrix rows verified first-hand — task 0.4 step 18 (2026-11-15)

Only rows whose pinned source was actually opened are listed. Anything not
listed stays "as specified, not yet re-verified" — no row may be promoted on
the strength of SPEC's own text (AGENTS rule 26).

| §1A.5 row | Status | First-hand evidence |
|---|---|---|
| Camera driver → **adopt** R2 | verified | pinned 2.1.9 in `managed_components/`; facts in the camera entries above; single-copy build check passes (AGENTS rule 33) |
| Framework → **reject** Arduino | verified | R1 compiles only as an Arduino sketch: `arduino-cli` build = 1,734,516 B (88 %) flash / 102,888 B (31 %) RAM vs VisionCam IDF build 387,356 B (24.6 %) — R1 has ≈231 KB free, no room to grow (`docs/HARDWARE.md` §3.2) |
| Frame buffers (R1: 4 × PSRAM, `GRAB_LATEST`) | verified | R1 `mjpeg2sd.cpp:20` `#define FB_CNT 4`, `:1070` `config.grab_mode = CAMERA_GRAB_LATEST`, `:1075` `config.fb_count = FB_CNT` — SPEC's claim is exact |
| SD mode → **adopt** 1-bit | verified | R1 `utilsFS.cpp:28` `use1bitMode = true`, `:80` `SD_MMC.begin("/sdcard", use1bitMode, ...)`; IDF side in the SDMMC entry above |
| Auth → **reject** Basic | verified | R1 `webServer.cpp:100-143`: pre-computes and caches the expected `Basic <b64>` value, then reads the request's `Authorization` header and compares — exactly the model SPEC replaces with sessions + CSRF |
| MQTT client → **adopt** esp-mqtt | verified | R1 `mqtt.cpp:14,36,46` uses `mqtt_client.h` / `esp_mqtt_client_publish`; VisionCam pins `espressif/mqtt@1.1.0` (`dependencies.lock`) and it builds |
| Camera init order → **adapt** | verified (source side) | R1 `initCam` sequence read; R2-side retry facts in the entry above. On-hardware behaviour `UNVERIFIED` |
| Motion cadence → **adopt/adapt** | verified | see Motion entry above (R1 `mjpeg2sd.cpp`, `motionDetect.cpp` line evidence) |

Rows **not** yet verified first-hand (still as-spec): AVI container layout,
NULL-frame recovery, playback timing, timelapse, free-space/retention, upload,
WebDAV, HA discovery, config store, logging, and every "defer/out of scope"
row. They are re-verified when their phase begins.

## Deliberate divergences from R1 — do not "fix" these back

| Area | R1 | VisionCam |
|---|---|---|
| Framework | Arduino sketch | Native ESP-IDF via PlatformIO (AGENTS rule 37) |
| Auth | Basic over HTTP, `strcmp` | Sessions, forced password change, salted hash, CSRF |
| UI delivery | Files on SD, downloaded from GitHub | Embedded in firmware, React + TS |
| Playback during capture | **Refused** (`mjpeg2sd.cpp:656`) | Never stalls recording; else `503 PLAYBACK_BUSY` |
| Crash safety | Header/index written only at close | Short segments + provisional header + boot recovery scanner |
| Motion during continuous recording | Disabled | Keeps running so events are marked |
| MQTT topics | `homeassistant/sensor/{hostname}` | `visioncam/{device_id}` stable IDs |
| Images over MQTT | Published on motion | Never by default; authenticated URL instead |
| Folder listing | Whole listing in one 32 KB `JSON_BUFF_LEN` buffer (`appGlobals.h:58`) | Paginated — conflicts with AGENTS rule 14 |
| Config | Key/value file + Preferences, save + reboot | NVS authoritative, versioned, live apply |
| Auto-format SD | Exposed as a parameter | Never (SPEC §2.3) |

## Evidence discipline

Every row above marked `UNVERIFIED` stays that way until a serial log or tool
output exists (AGENTS rule 26). R1's published performance numbers are
**targets**, not measurements, until reproduced on the owner's board —
`.opencode/plans/owner-decisions.md` D2.
