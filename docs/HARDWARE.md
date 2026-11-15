# Hardware baseline — VisionCam Pro

Target board: **AI-Thinker ESP32-CAM** (ESP32-D0WD, OV2640, 4 MB flash,
microSD, ESP32-CAM-MB programmer).

Everything in this file is either **measured on the owner's board** or
explicitly marked `UNVERIFIED`. Never promote a value to measured without a
serial log or tool output to back it (AGENTS rule 26).

---

## 1. Owner-decision context

| Decision | Answer | Date |
|---|---|---|
| D1 Licence | **A — clean-room**; R1 never copied or linked | 2026-11-15 |
| D2 R1 baseline | **Run during Phase 0** — procedure is section 3 below | 2026-11-15 |
| D3 FPS tolerance | **10 %** | 2026-11-15 |
| D4 TFLM | pending (needed by Phase 6) | — |
| D5 SD card model | pending (needed by Phase 9) | — |

---

## 2. Board facts (verified three ways, 2026-11-15)

Camera and SD pins are recorded in `firmware/include/board_profile.h`, which
is the only place pin constants may exist (AGENTS rule 6).

| Item | Value | Evidence |
|---|---|---|
| Camera pin map | PWDN 32, XCLK 0, SIOD 26, SIOC 27, Y9..Y2 = 35/34/39/36/21/19/18/5, VSYNC 25, HREF 23, PCLK 22 | SPEC §2.2 = R1 `camera_pins.h:187-206` = owner table |
| Flash LED | GPIO 4 | R1 `camera_pins.h:206` `LED_GPIO_NUM 4` |
| Status LED | GPIO 33 (red) | SPEC §2.2 cross-check note |
| SD pins | CLK 14, CMD 15, D0 2, D1 4, D2 12, D3 13 | owner table (SPEC §2.2 does not tabulate these) |
| SD mode | **1-bit** (CLK/CMD/D0 only) — frees GPIO4 and GPIO12 | SPEC §2.3, line 333; R1 README "MMC 1 line" |
| PSRAM | reported at boot by the smoke test | R1 README says 4 MB but warns clone boards differ — **measure** |
| Flash size | 4 MB (per `esp32cam` board definition) | `pio boards esp32cam` — confirm on hardware |

**Reserved pins:** GPIO16 (PSRAM), GPIO1 (U0T), GPIO3 (U0R).
**Boot strapping pins:** GPIO0 (boot), GPIO2, GPIO12 (flash voltage), GPIO15.

---

## 3. R1 baseline procedure (owner hardware, D2)

R1 is a **measurement tool only**. It is never VisionCam's build system and
its code is never copied (D1 = clean-room).

### 3.1 What you need

- AI-Thinker ESP32-CAM + ESP32-CAM-MB (or a USB-TTL on U0T/U0R)
- A microSD card you are willing to format (record the **exact model** — D5)
- **`arduino-cli` 1.6.0** with core `esp32:esp32` **3.3.12** installed
  (≥ 3.1.1 required by R1 README line 72) — *verified installed on this
  machine, 2026-11-15*
- The pinned R1 checkout at `reference/ESP32-CAM_MJPEG2SD-master/`
- Chrome (R1's README states browser functions are only fully tested there)
- A stable 5 V supply — R1 reports `Crash loop detected` on inadequate power

### 3.2 Compile check — **VERIFIED 2026-11-15** (no hardware needed)

This step proves the toolchain before you touch the board. It was run and
passed on 2026-11-15:

```bash
# IMPORTANT: compile from a COPY. reference/ is hash-pinned evidence and
# must never be modified (reference/fetch_references.sh --verify would fail).
mkdir -p /tmp/opencode/r1build
cp -r reference/ESP32-CAM_MJPEG2SD-master /tmp/opencode/r1build/ESP32-CAM_MJPEG2SD

arduino-cli compile \
  --fqbn esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=min_spiffs \
  /tmp/opencode/r1build/ESP32-CAM_MJPEG2SD
```

Observed result (exit 0):

```
Sketch uses 1734516 bytes (88%) of program storage space. Maximum is 1966080 bytes.
Global variables use 102888 bytes (31%) of dynamic memory, leaving 224792 bytes for local variables.
```

Notes proven by this run:

- R1's `ESP32-CAM_MJPEG2SD.h` ships with `CAMERA_MODEL_AI_THINKER` **already
  enabled** at line 17, inside `#if defined(CONFIG_IDF_TARGET_ESP32)`; the
  S3 model at line 33 is in the `#elif` branch, so **no edit is required** for
  ESP32. Do not "uncomment one" blindly — check which target branch you are in.
- `PartitionScheme=min_spiffs` is the correct token for R1's documented
  "Minimal SPIFFS" scheme (Arduino IDE label: *Minimal SPIFFS (1.9MB APP with
  OTA/128KB SPIFFS)*).
- R1 occupies **88 %** of its app slot — only ≈231 KB free. Expect no room for
  feature additions; this is a measurement tool only.

### 3.3 Build and flash on the board

1. Keep the copy from 3.2 (or make a fresh one).
2. If you prefer the Arduino IDE: install `arduino-esp32` ≥ 3.1.1 via Boards
   Manager, open the sketch, select **ESP32 Dev Module**, **PSRAM enabled**,
   partition **Minimal SPIFFS** (R1 README line 79 — not the "AI Thinker
   ESP32-CAM" board entry).
3. Compile — if it fails, update the `arduino-esp32` core to the latest v3.x
   (R1 README note).
4. Connect the ESP32-CAM-MB, put **GPIO0 to GND**, press RESET, upload, then
   **disconnect GPIO0 from GND and reset again** to boot normally.
5. Open Serial Monitor at **115200**.

Or with `arduino-cli` (same FQBN as 3.2, plus the port):

```bash
arduino-cli upload --fqbn esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=min_spiffs \
  --port /dev/ttyUSB0 /tmp/opencode/r1build/ESP32-CAM_MJPEG2SD
```

### 3.4 First run

1. On first install R1 starts in **Wi-Fi AP mode** — join the SSID
   `ESP-CAM_MJPEG_…`.
2. Browse to **`192.168.4.1`**, select your router, enter its password.
3. Let it download its `/data` folder to the SD card (needs internet on that
   step; the camera itself must work without internet afterwards).
4. Insert the microSD card if not already in. Known first-run failures:
   - `Startup Failure: Check SD card inserted` / `Camera init error 0x105`
     → wrong `CAMERA_MODEL_*` selection, not a hardware fault.
   - `Crash loop detected, check log` → inadequate power supply.

### 3.5 Measurements to record

Do these with **the same frame size, JPEG quality and SD card** you will later
use for VisionCam's parity test.

| # | Measurement | How |
|---|---|---|
| 1 | FPS per frame size × quality | R1 status page / serial log; record **VGA and QVGA at minimum** (SPEC §11.10), plus whatever else you can |
| 2 | SD write speed | write a segment, note MB/s from R1's log or time a known-size file copy |
| 3 | Free heap, minimum heap, largest free block | R1 serial log at idle **and** while recording |
| 4 | GPIO4/lamp with SD mounted | does the lamp light? R1 drives it low after mount (`utilsFS.cpp:81-84`) — confirm it stays **off** |
| 5 | Brownout / reset behaviour | note any `BROWNOUT` or unexpected reset in the log; try a weaker 5 V supply deliberately to see the symptom |
| 6 | Exact SD card model | printed on the card; note capacity + class |

### 3.6 Verification record

| Check | Status | Date | Evidence |
|---|---|---|---|
| `arduino-cli` 1.6.0 present | ✅ verified | 2026-11-15 | `arduino-cli version` |
| Core `esp32:esp32@3.3.12` installed (≥ 3.1.1 required) | ✅ verified | 2026-11-15 | `arduino-cli core list` |
| **R1 compiles unmodified** with `PSRAM=enabled,PartitionScheme=min_spiffs` | ✅ **verified** | 2026-11-15 | exit 0; 1,734,516 B (88 %) flash, 102,888 B (31 %) RAM |
| `CAMERA_MODEL_AI_THINKER` already enabled — no edit needed | ✅ verified | 2026-11-15 | `ESP32-CAM_MJPEG2SD.h:17`, inside the `CONFIG_IDF_TARGET_ESP32` branch |
| `reference/` tree unchanged after compiling | ✅ verified | 2026-11-15 | compiled from a `/tmp/opencode/r1build` copy |
| R1 flashed to the board | ⬜ not run | | needs the owner's hardware |
| Baseline table (section 4) filled | ⬜ not run | | needs the owner's hardware |

---

## 4. Baseline table (fill in — blocks performance claims)

> **Status: `UNVERIFIED` until filled.** Per SPEC §1A.7 step 4, no VisionCam
> performance claim may be marked verified without the matching row here.

| Frame size | JPEG quality | FPS | SD write MB/s | Free heap B | Min heap B | Largest block B | Notes |
|---|---|---|---|---|---|---|---|
| QVGA | | | | | | | UNVERIFIED |
| VGA | | | | | | | UNVERIFIED |
| SVGA | | | | | | | UNVERIFIED |
| XGA | | | | | | | UNVERIFIED |
| SXGA | | | | | | | UNVERIFIED |

| Item | Value | Status |
|---|---|---|
| SD card model / capacity / class | | UNVERIFIED |
| PSRAM size | | UNVERIFIED (smoke test reports it) |
| GPIO4 lamp with SD mounted | | UNVERIFIED |
| Brownout symptom observed | | UNVERIFIED |
| D3 FPS tolerance agreed | **10 %** | agreed 2026-11-15 |

---

## 5. VisionCam Phase 0 smoke test (once firmware builds)

1. `pio run -t upload` over the ESP32-CAM-MB (hold BOOT if auto-reset does
   not engage).
2. `pio device monitor` at 115200 — expect the banner from
   `firmware/src/main.cpp`: reset reason, PSRAM size, free/min heap, largest
   block.
3. Camera bring-up (task 0.3 step 11): sensor PID must report **OV2640**, a
   JPEG of plausible length must be captured, and a forced first-attempt
   failure must recover via PWDN (GPIO32) power-cycle.
4. SD mount (step 12): must mount in **1-bit**; test file must round-trip.
   `format_if_mount_failed` stays **false** — never auto-format.
5. GPIO4 test (step 13): confirm whether the ESP-IDF SDMMC 1-bit driver
   lights the lamp; if it does, drive GPIO4 low after mount and **record the
   result here**. R1's workaround was written for Arduino `SD_MMC`, so it must
   be re-proven on the IDF driver.
6. Camera + SD together, Wi-Fi **off** then **on** — no brownout, no reset.

| Smoke test step | Result | Date |
|---|---|---|
| Flash via ESP32-CAM-MB | UNVERIFIED | |
| Banner: reset reason / PSRAM / heap | UNVERIFIED | |
| Camera JPEG captured, PID = OV2640 | UNVERIFIED | |
| Retry + PWDN power-cycle recovers | UNVERIFIED | |
| SD mounts 1-bit, file round-trips | UNVERIFIED | |
| GPIO4/lamp behaviour documented | UNVERIFIED | |
| Camera + SD stable with Wi-Fi on | UNVERIFIED | |
| Brownout/reset behaviour recorded | UNVERIFIED | |

---

## 6. Power notes

- Use a stable 5 V supply sized for Wi-Fi TX peaks **and** camera operation.
- Brownout is the usual cause of boot loops on this board (R1 README).
- Do not power additional peripherals from GPIO pins.
