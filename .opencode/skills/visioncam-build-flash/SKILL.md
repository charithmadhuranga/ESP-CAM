---
name: VisionCam PlatformIO + ESP-IDF build and flash
description: Build, flash, monitor and debug VisionCam Pro firmware with PlatformIO on the ESP-IDF framework — pinned toolchain, board profile, partitions.csv, sdkconfig.defaults, component manager, size reports, serial monitoring and common ESP32-CAM bring-up failures. Load when running pio commands, changing platform/IDF versions, editing sdkconfig or partitions, chasing a build error, or flashing the AI-Thinker board over ESP32-CAM-MB.
license: MIT
metadata:
  version: "1.0"
  author: visioncam-pro
---

# Build, flash and monitor (PlatformIO `framework = espidf`)

Paths relative to the repository root. Read `AGENTS.md` first.

## Toolchain rules

- PlatformIO with **`framework = espidf` only** — never add the Arduino core
  without a recorded owner decision + ADR (AGENTS rule 37).
- **Pin everything**: `platform =` version, IDF version it provides, and every
  component in `firmware/src/idf_component.yml`. No `latest` (rule 24).
- The chosen platform + IDF versions live in `docs/adr/0001-toolchain.md`.
  Changing them requires a new ADR and a re-run of the Phase 0 camera smoke
  test.
- `espressif/esp32-camera` must be pinned to an exact version/commit and
  **only one copy** may ever be linked (rule 33). The registry reports 2.1.9
  while the upstream `library.json` says 2.0.0 — never trust a floating entry.

## Standard commands

```bash
pio run                              # build firmware
pio run -t size                      # flash/RAM size report
pio run -t clean                     # clean build
pio run -t upload                    # flash over ESP32-CAM-MB
pio device monitor                   # serial monitor
pio run -e <env>                     # specific environment
```

Web UI (run before firmware build so assets are embedded):

```bash
cd webui && npm ci && npm run build   # emits dist/ + asset manifest
python3 tools/embed_web_assets.py     # embed gzip+hashed assets into firmware
```

## Files that must stay in sync

| File | Contains | Rule |
|---|---|---|
| `platformio.ini` | platform, framework, board, envs, pinned libs | one env per supported target |
| `sdkconfig.defaults` | PSRAM on, flash/PSRAM 80 MHz, only OV2640, camera task core, DMA/JPEG buffer, log levels | under version control; symbol names verified against the pinned IDF |
| `partitions.csv` | app slots (+ OTA if used), NVS, data | sized for firmware **plus** embedded web assets; headroom recorded |
| `firmware/src/idf_component.yml` | `espressif/esp32-camera`, `esp-mqtt`, … | exact versions only |
| `firmware/include/board_profile.h` | AI-Thinker pin map | the only place pin constants exist |

## sdkconfig baseline (verify symbol names for the pinned IDF)

- PSRAM enabled; flash and PSRAM at 80 MHz.
- Camera menu: only OV2640 support enabled, explicit camera task core, DMA
  buffer size, JPEG buffer mode.
- `CONFIG_ESP_HTTPS_OTA`/OTA only if the two-slot partition table is chosen.
- Log levels: warn by default, per-module debug behind a config flag.

Option names change between ESP-IDF versions — verify each symbol, then
record the baseline. Never silently accept a default that differs.

## Board bring-up checklist (Phase 0 smoke test)

1. `pio run` succeeds with no ignored errors.
2. Flash via ESP32-CAM-MB (hold BOOT if the auto-reset circuit does not
   engage).
3. Serial log shows reset reason, PSRAM size, free/min heap, largest free
   block.
4. Camera init reports sensor PID = OV2640 and captures a JPEG of plausible
   length.
5. SD mounts in 1-bit mode; test file round-trips.
6. Camera + SD together, Wi-Fi off then on, no brownout/reset.

## Common failures

| Symptom | Likely cause | Check |
|---|---|---|
| `esp_camera_init` fails repeatedly | PSRAM not enabled, wrong pins, camera not seated | `board_profile.h`, `sdkconfig.defaults`, PWDN GPIO32 power-cycle |
| Boot loop / brownout | Undersized 5 V supply (R1 says this is the usual cause) | supply, not firmware |
| SD mount fails | 1-bit config, card format, GPIO4/12 conflict | `slot.width = 1`, `SDMMC_HOST_FLAG_1BIT`, card reformatted FAT32 |
| Lamp on with SD mounted | IDF SDMMC driver initialises GPIO4 | drive GPIO4 low after mount, record result in `docs/HARDWARE.md` |
| Two `esp32-camera` copies | component manager + PlatformIO lib both pulling it | pin one source, assert at build |
| Linker OOM / partition overflow | web bundle too large | `pio run -t size`, asset manifest, gzip, lazy-load pages |
| Watchdog reset in capture loop | NULL-frame spin | SPEC §9.1 rate-limited recovery + 10 ms yield |

## Size discipline

After every build record: firmware size, web asset size, free partition
headroom. If the UI bundle makes the partition impractical, do **not** ship a
development bundle — shrink it, or use a documented separate data partition.

## Definition of done for a build change

Compiles clean · size recorded · single `esp32-camera` copy ·
`sdkconfig.defaults` diff explained · ADR updated if the toolchain moved ·
Phase 0 smoke test re-run when the camera driver or IDF version changed.
