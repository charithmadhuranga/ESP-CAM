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
| D2 R1 baseline | **Run during Phase 0** — **executed 2026-11-15**: R1 flashed unmodified, driven over HTTP, FPS/heap/SD-speed/heap rows in §4. All three follow-ups **closed 2026-11-16**: GPIO4 lamp observed under R1 (§4), D5 answered (SanDisk 8 GB, no class marking), weak-supply drill skipped by owner decision | 2026-11-15 |
| D3 FPS tolerance | **10 %** | 2026-11-15 |
| D4 TFLM | pending (needed by Phase 6) | — |
| D5 SD card model | **genuine SanDisk 8 GB, no speed-class marking visible** (owner 2026-11-16); reformatted MBR+FAT32 same day (§4.2.1) | 2026-11-16 |

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
| PSRAM | **8,388,608 B (8 MB)** fitted; IDF maps **4 MB** of it (`Virtual address not enough for PSRAM, map as much as we can. 4MB is mapped`) | measured 2026-11-15 — `docs/evidence/phase0-smoke-boot-11-forced-retry.log`, lines `PSRAM: 8388608 bytes` and the `esp_psram` warning. R1 README's 4 MB guess is **wrong for this board** |
| Flash size | **4 MB** | measured 2026-11-15 — boot log `SPI Flash Size : 4MB` (same evidence file) |

> **PSRAM finding (2026-11-15):** the module carries 8 MB, but the original
> ESP32 maps only ~4 MB of it into the linear virtual-address window, and the
> boot heap pool shows 4,192,024 B PSRAM free / 4,128,768 B largest block.
> Planning numbers (frame buffers ≈2 MB → SVGA, ≈4 MB → UXGA) stay unchanged,
> but §4 records the real figures rather than the README's assumption.

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
| R1 flashed to the board | ✅ **verified** | 2026-11-15 | `arduino-cli upload` exit 0, `Hash of data verified`, R1 banner `ESP-CAM_MJPEG 10.10.1` on serial |
| R1 joins the owner's LAN (measured over HTTP from this PC) | ✅ verified | 2026-11-15 | `Wifi Station IP, use 'http://192.168.43.171'`, `/status` HTTP 200 (procedure 3.7) |
| Baseline table (section 4) filled | ✅ **verified** | 2026-11-15 | 8 recording runs; `docs/evidence/phase0-d2-r1-avi-stats-blocks.txt` |
| No brownout / panic / WDT during the whole R1 run | ✅ verified | 2026-11-15 | `grep` for `BROWNOUT|panic|WDT|rst:0x` over `phase0-d2-r1-serial-run.txt` → 0 hits; `printResetReason: Power on reset` |
| GPIO4/lamp observed under R1 (§3.5 item 4) | ✅ **verified — owner observed** | 2026-11-16 | lamp flashed in the boot window **and** repeated blink afterwards; R1 timeline: SD mount `00:00:00.572`, lamp feature init `00:00:01.277` (`setupLamp`) — see §4 lamp rows; `docs/evidence/phase0-d2-r1-lamp-run.log` |
| Exact SD card model (§3.5 item 6) | ✅ **verified** | 2026-11-16 | owner: genuine **SanDisk 8 GB**, **no speed-class marking visible** → D5 recorded as such |
| Weak-supply brownout drill (§3.5 item 5) | ⏭️ **skipped — owner decision** | 2026-11-16 | owner chose to skip; normal-supply behaviour verified brownout-free on both builds (boots 10–12, full D2 run) |

### 3.7 Provisioning R1 onto the LAN (how section 4 was measured)

R1's own setup page normally collects the router credentials, but joining the
ESP32's AP from this workstation would drop its internet, so the credentials
were written straight into NVS instead. `prefs.cpp:216-223` shows R1 reads
`ST_SSID` / `ST_Pass` from the Preferences namespace **`ESP-CAM_MJPEG`**
(its `APP_NAME`) whenever its SD config holds no SSID — i.e. exactly the
first-boot state.

```bash
pio run -d tools/nvs_provision -t upload          # our helper, ESP-IDF, pinned
/home/charith/.platformio/penv/bin/python3 /tmp/opencode/provision.py
#   -> cfgtool: OK — committed and verified (pass hidden)
pio run -d tools/nvs_provision -t upload          # (optional) `join` command
#   -> cfgtool: JOIN OK ... rssi -47, pmf:0
arduino-cli upload --fqbn esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=min_spiffs \
  --port /dev/ttyUSB0 /tmp/opencode/r1build/ESP32-CAM_MJPEG2SD
```

- The passphrase is read from NetworkManager and sent only to the UART; it is
  never echoed, logged, or put on a command line (AGENTS rule 17). The tool
  firmware prints lengths only. Nothing credential-bearing is committed.
- Clean-room note (D1): `tools/nvs_provision` contains **no R1 code** — it
  writes two documented NVS keys, the machine equivalent of typing the
  credentials into R1's own setup page.
- `/status` and `/control` were open (R1's `Auth_Pass` is empty), so every
  measurement below was driven by `curl` while a passive serial capture
  recorded R1's own stats output.

---

## 4. Baseline table — **MEASURED on the owner's board 2026-11-15**

R1 10.10.1 flashed **unmodified** and driven over HTTP while a passive 115200
capture recorded R1's own stats block at every recording close. Every number
below is a verbatim line from
`docs/evidence/phase0-d2-r1-avi-stats-blocks.txt` (8 runs). Per SPEC §1A.7
step 4 these rows are now the acceptance bar — **no VisionCam performance
claim may be marked verified without its matching row here.**

| Frame size | JPEG quality | FPS | SD write MB/s | Free heap B | Min heap B | Largest block B | Notes |
|---|---|---|---|---|---|---|---|
| QVGA 320×240 | 12 | 30.8 (req 30) | 0.51 (520 kB/s) | 72,024 | 53,504 | 42,996 | 189 frames, 997 KB AVI, busy 40 %, store 10 ms/frame |
| VGA 640×480 | 12 | 20.6 (req 20) | 0.50 (513 kB/s) | 75,072 | 53,504 | 42,996 | 70 frames, 1.0 MB AVI, busy 67 %, store 29 ms/frame |
| SVGA 800×600 | 12 | 20.4 (req 20) | 0.49 (502 kB/s) | 74,008 | 53,504 | 42,996 | busy **100 %** — SD becomes the bottleneck |
| XGA 1024×768 | 12 | 5.3 (req 5) | 0.49 (499 kB/s) | 72,404 | 53,504 | 42,996 | busy 52 % (R1 drops FPS to 5 at XGA) |
| SXGA 1280×1024 | 12 | 6.2 (req 5) | 0.49 (507 kB/s) | 74,376 | 53,504 | 40,948 | busy 98 %, largest block 2 KB lower |
| VGA | 8 | 20.7 | 0.48 (493 kB/s) | 72,328 | 53,504 | 42,996 | quality sweep, run 7 |
| VGA | 16 | 20.4 | 0.50 (515 kB/s) | 73,808 | 53,504 | 42,996 | quality sweep, run 8 |

- R1's own unit is **kB/s** (`vidSize / wTimeTot`); the MB/s column is that
  figure ÷ 1024 — no silent unit conversion was applied.
- "Min heap" is `ESP.getMinFreeHeap()` — the **lifetime** minimum, so it is the
  same value on every row: it reached 53,504 B during the first QVGA run and
  never went lower.
- Frame-size indices sent to `/control` (R1's 0-based `framesize_t`, same order
  as `framesize_t`: QVGA 6, VGA 10, SVGA 11, XGA 12, SXGA 14; R1's shipped
  default was 10 = VGA, quality 12.
- Every recording was sized to fit the card's **5,216,256 B** FAT volume
  (§4.2) — 1.3 MB free at the start, each AVI deleted immediately after its
  stats block was captured, `/status` checked afterwards. `free_bytes` never
  reached 0 on any row used above.

### 4.1 R1 heap figures

| Point | Free heap B | Largest block B | Min heap B | pSRAM B | Status |
|---|---|---|---|---|---|
| R1 at boot (`checkMemory`) | 62,348–67,356 (3 boots, varies) | 38,900–42,996 | 62,316–67,184 | 1,752,776 | measured 2026-11-15 |
| R1 idle, HTTP + camera running | 70,000–74,000 (`/status` samples) | — | — | ~1.19 MB | measured 2026-11-15 |
| R1 at recording close (table above) | 72,024–75,072 | 40,948–42,996 | 53,504 | 914,892 | measured 2026-11-15 |

### 4.2 Storage finding — CSD 7.5 GiB vs a 5.0 MB FAT volume

> **RESOLVED 2026-11-16 — see §4.2.1.** The card shipped with a bootable
> Linux Mint image; the owner authorised a reformat and
> `tools/sd_format/` rewrote it as MBR + FAT32 across the whole card
> (8,051,032,064 B usable, 99 % of the CSD). The finding below is kept as
> the record of what was observed and why.

Three independent numbers from the **same card**, and only one of them is
what a filesystem can use:

| Layer | Number | Source |
|---|---|---|
| CSD capacity | **8,053,063,680 B** (7.5 GiB) | `sdmmc_card_print_info` (VisionCam + R1's `infoSD`) |
| CID product name | `SD16G` (implies 16 GB) | same print, `Name: SD16G`, `Type: SDHC`, `Size: 7680MB` |
| FAT volume | **5,216,256 B usable, 1,349,632 B free, cluster 2048 B × 2547 clusters** (FAT12) | arduino-esp32 `SD_MMC.totalBytes()` *and* IDF `f_getfree` — identical |

The two FATFS implementations agree to the byte, so **the 5 MB is in the
card's own FAT BPB, not in either wrapper**: the formatted volume simply does
not cover the card. The card holds `/boot`, `/EFI` (≈3.5 MB) and R1's `/data`
(≈0.2 MB) — nothing more can be stored on it until the owner replaces or
reformats it. Evidence:
`docs/evidence/phase0-d2-vc-fat-crosscheck-boot.log` (IDF view).

**Consequences recorded for later phases — read before Phase 3:**

1. **R1's stats block cannot be trusted while the disk is full.** R1 counted
   bytes it *tried* to write: one probe reported `File size: 12.3MB`,
   `Actual FPS: 20.1`, `Average SD write speed: 3796 kB/s` while the file on
   the card was really **1,349,632 B** (the directory entry and the HTTP
   download both said 1.3 MB, and `used_bytes` went 3.7 → 5.0 MB). Cause,
   studied in the pinned source: `saveFrame()` adds to `vidSize`/`frameCnt`
   unconditionally and never checks `aviFile.write()`'s return value
   (`mjpeg2sd.cpp:239-261`), so short writes are invisible. That run's 3796
   kB/s is **discarded**; the row table above comes from runs that fit.
2. **VisionCam's recorder (Phase 3) must check write return values** and
   abort/truncate the segment on a short write rather than counting attempts
   into the FPS/speed figures. A counter that is incremented before the write
   result is known would reproduce exactly this defect.
3. **Phase 9 retention planning cannot be sized on this card.** With 5 MB
   usable there is no room for retention at VGA; a working card is needed
   before the phase-exit numbers exist. VisionCam never auto-formats
   (SPEC §2.3), so the reformat is an owner action. — *Superseded by
   §4.2.1: the card was reformatted 2026-11-16 and now has room; the
   "never auto-format" rule itself is unchanged.*

### 4.2.1 Resolution — card reformatted MBR + FAT32 (2026-11-16)

The owner identified the card: a genuine **SanDisk 8 GB** carrying a
bootable **Linux Mint** image (owner, 2026-11-16), and instructed that it
be rewritten with a suitable file system and partition table. That
explained the §4.2 geometry exactly — the "5 MB FAT volume" was the
image's **EFI system partition** (`/EFI`, `/boot`), and the rest of the
card was an **ext4** root filesystem FATFS cannot see. The pre-format
partition table read by the tool confirms it:

```
p2: type 0xEF (EFI system), start LBA 8452, 10240 sectors = 5242880 B
p3: type 0x83 (Linux),      start LBA 6041600, 9687040 sectors = 4959764480 B
```

VisionCam itself **never formats a card** (SPEC §2.3 keeps
`format_if_mount_failed = false`), so the rewrite was done by the
standalone one-shot tool `tools/sd_format/` (flash, run `format`, type
`ERASE`, reflash firmware — never part of a release build). FatFs
`f_mkfs` (`FM_ANY`, `n_fat=2`) from the pinned ESP-IDF 6.1.0 wrote:

| Layer | Result |
|---|---|
| Partition table | MBR, one entry: `p1: type 0x0C (FAT32 LBA), start LBA 63, 15,728,577 sectors = 8,053,031,424 B` |
| Filesystem | FAT32, two FATs, 32,768 B clusters × 245,698 |
| Usable volume | **8,051,032,064 B** (99 % of the 8,053,063,680 B CSD — no coverage warning) |

- Format duration **2,791 ms**; post-format self-test (FatFs layer)
  wrote, read back and deleted `VCSDFMT.TXT` — OK.
- Evidence: `docs/evidence/phase0-sd-format.log` (pre-format `info` with
  the Mint table, typed `ERASE`, `FORMAT OK`, post-format `info`).
- VisionCam reflash after the format: `FAT volume: 8051032064 B usable
  … CSD capacity 8053063680 B`, no coverage warning, `sd: OK`, full
  verdict line green, single `POWERON_RESET` —
  `docs/evidence/phase0-smoke-boot-12-postformat.log`.
- The Mint image is **destroyed** (owner instruction); recovery means
  re-imaging the card on a PC.

| Item | Value | Status |
|---|---|---|
| SD card identity as seen by the driver | `Name: SD16G`, `Type: SDHC`, `Size: 7680MB`, CSD v2, 15,728,640 sectors | measured 2026-11-15 (smoke boot 4) |
| SD card printed label / class | **SanDisk, genuine, 8 GB; no speed-class marking visible** on the card | owner-read 2026-11-16 → **D5 closed** (class recorded as "no marking"; Phase 9 measures the card's real throughput anyway) |
| FAT volume usable on this card | **8,051,032,064 B (7.50 GiB), 8,050,999,296 B free, 32 KB clusters** — reformatted MBR+FAT32, see §4.2.1 | measured 2026-11-16 (`phase0-sd-format.log`, re-confirmed by boot 12) |
| PSRAM size | **8,388,608 B fitted; 4,192,024 B in the free heap pool, largest block 4,128,768 B; 4 MB mapped** | measured 2026-11-15 |
| GPIO4 lamp with SD mounted | **OFF for the whole boot** — the ESP-IDF SDMMC 1-bit driver does *not* light the lamp (owner observed a full boot, 2026-11-15) | verified 2026-11-15 (VisionCam build) |
| GPIO4 lamp under R1 / Arduino `SD_MMC` | **Owner observed 2026-11-16:** a flash in the boot window **and** a repeating blink afterwards. R1's own timeline separates the two: SD mounts at `00:00:00.572`, but R1's lamp feature does not exist until `setupLamp` at `00:00:01.277` — and `setLamp()` then forces Medium brightness whenever `!appSetupDone`, so the later repeating blink is R1's **status pattern** (setup incomplete: Wi-Fi association failed, `Need to connect to AP to setup router`), **not** SD activity. The early flash is consistent with Arduino `SD_MMC` initialising pin 4 during mount — exactly R1's own workaround comment (`utilsFS.cpp:81-84`) — but the two events are only 0.7 s apart, so vision alone cannot fully isolate them. What **is** proven on hardware: VisionCam's ESP-IDF driver leaves the lamp **OFF for the whole boot** (2026-11-15) while R1 lights it during boot — the stacks differ, and VisionCam's post-mount drive-low stays as belt-and-braces | ✅ observed 2026-11-16 | `docs/evidence/phase0-d2-r1-lamp-run.log` (mount + `setupLamp` timestamps) |
| Brownout symptom observed (VisionCam build) | **none** — single `POWERON_RESET`, no `BROWNOUT`/panic/WDT across the full smoke run incl. 3 s of Wi-Fi beacon TX | verified 2026-11-15 (smoke boot 11); still none on boot 12 (2026-11-16) |
| Brownout / reset behaviour (R1 run) | **none in the whole D2 run** — 8 recordings, HTTP + serial traffic, `printResetReason: Power on reset`, zero `BROWNOUT`/panic/WDT lines | verified 2026-11-15; weak-supply drill **skipped by owner decision 2026-11-16** (§3.6) |
| D3 FPS tolerance agreed | **10 %** | agreed 2026-11-15 |

---

## 5. VisionCam Phase 0 smoke test — **VERIFIED on hardware 2026-11-15**

Raw serial logs are committed under `docs/evidence/` (ASCII-sanitised copies
of the capture tool's output; nothing was edited out).

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

| Smoke test step | Result | Date | Evidence |
|---|---|---|---|
| Flash via ESP32-CAM-MB | **verified** — auto-reset works, no BOOT press needed; app at `0x20000` boots, partition table matches `partitions.csv` | 2026-11-15 | boot 11 log, `rst:0x1 (POWERON_RESET)` + `boot:` partition listing |
| Banner: reset reason / PSRAM / heap | **verified** — `reset reason: POWERON`, `PSRAM: 8388608 bytes`, heap boot free 4,448,703 B / min 4,416,672 B / largest 4,128,768 B | 2026-11-15 | boot 11 log |
| Camera JPEG captured, PID = OV2640 | **verified** — `sensor PID 0x0026 = OV2640`, 3 warm-up frames discarded, `JPEG grabbed: 243471 B (1600x1200)`, runtime dropped to VGA | 2026-11-15 | boot 11 log |
| Forced first-attempt failure → retry recovers | **verified** — attempt 1 sabotaged with `pin_xclk = -1` (real failure: `ESP_ERR_NOT_SUPPORTED`), attempt 2 after `cameraShutdown()` + PWDN power-cycle → `forced-failure test: err=ESP_OK, attempts=2 -> OK` | 2026-11-15 | boot 11 log. *Method note:* holding PWDN instead **does not work** — R2's `camera_probe` releases PWDN itself each init (`esp_camera.c:204-216`), proven on hardware when the hold attempt still passed at `attempts=1` |
| SD mounts 1-bit, file round-trips | **verified** — `SD mounted at /sdcard, negotiated 20000 kHz`, `SSR: bus_width=1`, `SD self-test OK: wrote, read back and deleted /sdcard/vc_smoke.txt` | 2026-11-15 | boot 8/10/11 logs |
| GPIO4/lamp behaviour documented | **verified — lamp stayed OFF for the entire boot** (owner watched a full boot). The ESP-IDF SDMMC 1-bit driver does **not** light GPIO4; our post-mount drive-low is belt-and-braces | 2026-11-15 | owner observation + boot 9/10 logs |
| Camera + SD stable with Wi-Fi on | **verified** — SoftAP `visioncam-smoke` ch6 up, scan heard 2 networks, camera+SD OK both `wifi off` and `wifi on` (`step 14 result: OK`), no instability | 2026-11-15 | boot 10/11 logs |
| Brownout/reset behaviour recorded | **verified for this run** — exactly **1** boot in the whole capture, `POWERON_RESET`, no `BROWNOUT`/panic/WDT even across 3 s of beacon TX + scan. The weaker-supply drill stays owed with D2 | 2026-11-15 | boot 11 log |

Final verdict line (boot 11):

```
camera: OK | sd: OK | psram: OK | wifi(step14): OK | forced-retry: OK
```

### 5.1 Known hardware quirk — SD card contact

During this session the SD mount failed on **six of the first seven boots**
with *varying* errors: `ESP_ERR_TIMEOUT` (no reply at all, `sdmmc_cmd.c` CMD
timeout) and `ESP_ERR_INVALID_RESPONSE` (card answered but the CMD8 echo
pattern was wrong, `sdmmc_cmd.c:136`). Varying corruption → silence is the signature of
**marginal contact or power at the slot**, not a firmware bug (a code bug
fails deterministically: it failed identically every time). Re-seating the
card fixed it immediately and it then passed on boots 8–11 without a single
failure.

**If mount errors vary between runs, re-seat the card (push until it clicks)
before debugging firmware.** Record the exact card label here when read — D5.

---

## 6. Power notes

- Use a stable 5 V supply sized for Wi-Fi TX peaks **and** camera operation.
- Brownout is the usual cause of boot loops on this board (R1 README).
- Do not power additional peripherals from GPIO pins.

## 7. VisionCam first-run Wi-Fi setup

When no Wi-Fi SSID has been saved, VisionCam starts a WPA2 setup access point:

- SSID: `VisionCam-XXXXXX` (suffix is derived from the board MAC)
- Passphrase: `visioncam`
- Setup page: `http://192.168.4.1`

Join the camera AP from a phone or PC, sign into VisionCam, then open
**Settings → Connect camera to Wi-Fi**. Enter the 2.4 GHz network name and
password. The firmware tries to obtain an IP before staging the credentials.
If the join fails, the setup AP remains available and the candidate is not
stored. After a successful join, use **Restart camera** in Settings. On the
next boot the firmware saves the staged credentials to NVS before camera
initialization, then connects as a station to the configured Wi-Fi network.
The firmware now closes the setup AP and restarts automatically after sending
the successful setup response.

The Wi-Fi password is write-only and is not included in API responses or
serial logs. First-login still requires changing the default device password.
The web-UI provisioning flow has not yet been exercised on the physical board.

**2026-11-21 setup-AP check:** the board had saved Wi-Fi credentials, so it
initially booted in station mode without advertising the setup AP. After
clearing only the saved Wi-Fi credential keys, the flashed firmware logged
the AP at `192.168.4.1`, and a workstation Wi-Fi scan detected the
`VisionCam-XXXXXX` network. The workstation was not joined to the AP.
**2026-11-21 serial provisioning check:** the active workstation Wi-Fi profile
was written to NVS over UART using a temporary provisioner built with the
firmware's exact partition layout. The normal firmware then loaded the saved
credentials and reported `GOT_IP 10.151.183.4` (RSSI -55 dBm). The workstation
was on the same `/24`, but an HTTP request to the camera timed out; access
from this phone hotspot may be isolated between clients and is not verified.
The credential value was not printed or recorded.

**2026-11-21 AP shutdown update:** firmware now switches to station-only mode
after a successful `/wifi/config` response, then restarts to commit the staged
credentials. A later configured-boot check reported `GOT_IP 10.151.183.4`
(RSSI -51 dBm), and the workstation scan did not show the setup SSID. The
successful setup-endpoint-to-AP-shutdown transition has not yet been exercised
on hardware.
