# ADR-0001: Toolchain, ESP-IDF version and build layout

- **Status:** Accepted — 2026-11-15
- **Deciders:** Owner (IDF version choice), implementer (pins and layout)
- **Context:** Phase 0, plan `01-phase-0-baseline.md` task 0.2. SPEC §12
  Phase 0 requires a pinned, reproducible build; AGENTS rules 24 and 37
  forbid floating versions and the Arduino layer.

## Decision

| Item | Choice | Pin |
|---|---|---|
| Build system | PlatformIO Core 6.2.0 | installed version, machine-local |
| Platform | `platformio/espressif32` | **8.0.0** (published 2026-10-19) |
| Framework | `framework = espidf` (native ESP-IDF, **no Arduino**) | resolved version recorded below |
| ESP-IDF | **6.1.0** (owner decision, 2026-11-15) | `framework-espidf ~4.60100.0` |
| Board | `esp32cam` (AI Thinker ESP32-CAM) | 4 MB flash, 240 MHz |
| Language | C++ (`firmware/src/main.cpp`) | per AGENTS coding standards |
| Camera driver | `espressif/esp32-camera` | **2.1.9**, commit `b3f7fe802f41f17fd7a104d517c53f1f62c8414d` |
| MQTT | `espressif/esp-mqtt` | pinned in Phase 0 task 0.2 step 8 |

### Why ESP-IDF 6.1.0

Owner chose it over the conservative IDF 5.5.5 option on 2026-11-15.

- Platform 8.0.0's default `framework-espidf ~4.60100.0` resolves to IDF
  6.1.0. PlatformIO's version encoding is `<pkg>.<IDF Mmmpp>.<patch>`, so
  `60100` = 6.1.0 (calibrated against known releases: `40408` = 4.4.8,
  `50503` = 5.5.3, `60001` = 6.0.1).
- Classic **ESP32 remains supported** in IDF 6.x — Espressif's stable docs
  still publish an `esp32` target and v6.0 *added* chips rather than dropping
  the ESP32.
- R2 declares `idf: ">=5.1"` with no upper bound; the registry page for
  2.1.9 shows "Supports all targets" and the component was uploaded
  2026-11-09, i.e. built against this generation.
- Greenfield code has no 5.x→6.x migration cost.

**Documented fallback:** if the Phase 0 camera smoke test fails for reasons
traceable to IDF 6 (removed legacy drivers — sigma-delta, HSPI/VSPI IOMUX
macros), pin `platformio/framework-espidf@4.50505.0` (IDF 5.5.5) via
`platform_packages`, update this ADR, and re-run the smoke test.

### Version encoding reference

`framework-espidf` versions seen in the registry (2026-11-15):

| Package version | ESP-IDF |
|---|---|
| `3.40408.0` | 4.4.8 |
| `3.50503.0` | 5.5.3 |
| `4.60001.0` | 6.0.1 |
| `4.60100.0` | 6.1.0 ← our default |
| `4.50505.0` | 5.5.5 ← fallback |

## Build layout

ESP-IDF owns the source layout, so PlatformIO's defaults are redirected
rather than fought:

```
platformio.ini            src_dir = firmware/src, include_dir = firmware/include
CMakeLists.txt            root ESP-IDF project file + rule 33 single-copy check
firmware/include/         board_profile.h — the only place pins exist (rule 6)
firmware/src/CMakeLists.txt   idf_component_register(...)
firmware/src/idf_component.yml  pinned components (R2 2.1.9, mqtt 1.1.0)
dependencies.lock         committed exact-version lock (incl. transitive)
managed_components/       downloaded by the component manager — gitignored
sdkconfig.defaults        verified symbol baseline (see below)
partitions.csv            two 1.5 MB OTA slots + 884 KB storage (see below)
                          platformio.ini must ALSO set
                          board_build.partitions — see "two-pipeline trap"
```

Notes from the PlatformIO ESP-IDF docs that constrain this layout:

- **No whitespace in project paths** — `/home/charith/Projects/ESP-CAM`
  complies.
- **`src_filter` cannot be used** under `framework = espidf`; sources are
  listed explicitly in `idf_component_register`.
- The root `CMakeLists.txt` boilerplate order is fixed and must not be
  reordered.
- Renaming the IDF `main` component (we use `src`) means dependencies must be
  declared explicitly with `REQUIRES` when a component needs one.

## Pinned components (task 0.2 steps 7–8)

`firmware/src/idf_component.yml` is the source of truth; the ESP-IDF
component manager resolves it into `managed_components/` and locks the
result in `dependencies.lock` (**committed** — it is the exact-version pin
for transitive dependencies; `managed_components/` is ignored as build
input).

| Component | Pinned | Why | Verified |
|---|---|---|---|
| `espressif/esp32-camera` | **2.1.9** | R2, the runtime camera driver (SPEC §1A.3). Registry 2.1.9 == upstream commit `b3f7fe802f41f17fd7a104d517c53f1f62c8414d`, matching `reference/pins.txt`. | ✅ downloaded, `esp_camera.c`/`cam_hal.c`/`sensor.c`/`sensors/ov2640.c` compile; `libespressif__esp32-camera.a` produced |
| `espressif/mqtt` | **1.1.0** | Phase 7 MQTT client. **Registry path is `espressif/mqtt`** — `espressif/esp-mqtt` returns HTTP 404. Requires `idf >= 5.3`; satisfied by 6.1.0. | ✅ downloaded, `libmqtt_outbox`/`libmqtt_utils` archive |
| `espressif/esp_jpeg` | **1.3.2** (transitive) | Required by esp32-camera. Its manifest declares the *range* `^1.3.1`, so this is the one place a floating range exists — `dependencies.lock` pins the resolved 1.3.2 and must be committed. | ✅ in `dependencies.lock` |

Notes:

- **SPEC §5's "esp-mqtt client (3.1.1)" is the MQTT *protocol* version**, not
  a component version — no conflict with pinning 1.1.0.
- The upstream `library.json` inside the esp32-camera source tree still says
  `2.0.0`; upstream lags its own release tag, so never read a version from
  `library.json` (rule 24). The manifest `version:` and `dependencies.lock`
  are authoritative.
- Neither component is linked into the app **yet** — `firmware/src` does not
  `REQUIRE` them, which is why RAM/Flash figures are unchanged (13,932 /
  183,784 B). They are compiled and archived, ready for the camera manager.

### Rule 33: single-copy check (task 0.2 step 7)

The root `CMakeLists.txt` runs a check on **every configure**: it walks
`BUILD_COMPONENT_TARGETS`, skips anything under `IDF_PATH`, and counts the
components that provide `esp_camera.h`. Zero copies → fatal (the pin failed
to resolve); more than one → fatal, listing every offending path.

Both paths were exercised, not assumed:

| Test | Result |
|---|---|
| Normal configure | ✅ `count=1`, build SUCCESS |
| Fixture `components/vc_test_dup/` with a copy of `esp_camera.h` | ✅ **FAILED** with `AGENTS rule 33 violated: 2 copies …` naming both directories; fixture removed, build green again |

## Partition table (task 0.2 deliverable 5)

4 MB flash, two-slot OTA plus a raw data partition. Source of truth is
`partitions.csv` at the repo root; the byte map and rationale live in its
header.

| Name | Type/SubType | Offset | Size | Purpose |
|---|---|---|---|---|
| `nvs` | data/nvs | `0x009000` | 92 KB | config, credentials, versioned schemas |
| `ota_0` | app/ota_0 | `0x020000` | 1.5 MB | app slot A — boots first |
| `ota_1` | app/ota_1 | `0x1a0000` | 1.5 MB | app slot B |
| `otadata` | data/ota | `0x320000` | 8 KB | which slot boots + rollback state |
| `phy_init` | data/phy | `0x322000` | 4 KB | RF calibration |
| `storage` | data/spiffs | `0x323000` | 884 KB | models / UI fallback (raw until used) |

Verified against the pinned IDF 6.1.0 `gen_esp32part.py`: no overlaps, app
offsets `0x10000`-aligned, app sizes `0x1000`-aligned, all data offsets
`0x1000`-aligned, and the last partition ends at exactly `0x400000`.

**Why two app slots when no OTA code exists yet:** the partition table cannot
be changed later without erasing the whole device, so the layout keeps the OTA
option open (SPEC §5.4 allows firmware update only once a safe OTA design
exists). Each slot is 1.5 MB — 4× the current firmware — so nothing is given
up today. `otadata`/`phy_init` sit after the app slots only because app
offsets must be `0x10000`-aligned and the standard `0x9000`–`0x12000` header
area would otherwise strand 56 KB.

### The two-pipeline trap (why `partitions.bin` was stale)

There are **two independent partition pipelines** and they do not share a
source of truth:

| Pipeline | Driven by | Output | Consumed by |
|---|---|---|---|
| IDF/CMake (configure time) | `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` in `sdkconfig.defaults` | `flash_args`, `app-flash_args` | `idf.py flash` |
| PlatformIO/SCons (build time) | `board_build.partitions` in `platformio.ini` | `partitions.bin`, `ESP32_APP_OFFSET`, `ota_data_initial.bin` | **`pio run -t upload`** |

`sdkconfig.h` correctly reported `CONFIG_PARTITION_TABLE_CUSTOM 1` and
`CONFIG_PARTITION_TABLE_FILENAME "partitions.csv"`, yet
`.pio/build/esp32cam/partitions.bin` still held the **old** `nvs/phy/factory
1M` table. Cause: `espidf.py`'s `generate_partition_table()` reads
`board.get("build.partitions", "partitions_singleapp.csv")`, and the
`esp32cam` board JSON has no `build.partitions` key — so PlatformIO silently
fell back to the framework's default CSV. Setting
`CONFIG_PARTITION_TABLE_CUSTOM*` cannot influence it.

Left unfixed, `pio run -t upload` would have flashed the app at `0x10000`
(the `factory` offset) — inside our NVS region — and written a partition table
at `0x8000` that disagreed with the app image. The device would not have
booted.

Fix: `board_build.partitions = partitions.csv` in `platformio.ini`.

Also note: PlatformIO builds through **SCons**, so ninja's
`partition_table/partition-table.bin` custom command (whose rule *does* list
`partitions.csv` as an input) never executes, and `flash_args` — an IDF-side
file — keeps referencing that never-generated path. PlatformIO's upload path
does not read `flash_args` at all (verified: no reference in
`platformio/` core or the platform's `builder/`), so this is harmless here,
but do not flash with `idf.py` expecting it to work.

**Both pipelines verified to agree**, not assumed: the IDF-side rule was
re-run by hand with its exact arguments and `cmp`-ed against
`partitions.bin` — byte-identical (md5 `7ca60f29f1fa15cf0b949d746387d5da`).
`parttool.py … --partition-boot-default` returns `0x180000 0x20000`, i.e.
`ESP32_APP_OFFSET` = `0x20000` (our `ota_0`), and the size-check denominator
moved from 1,048,576 B (old 1 MB `factory`) to 1,572,864 B (1.5 MB `ota_0`).

## sdkconfig symbol verification

ESP-IDF renames Kconfig symbols between releases, and **an unknown symbol in
`sdkconfig.defaults` is ignored silently** — a stale name fails without an
error. Therefore each symbol is grepped against the pinned framework's
Kconfig and the result is recorded here.

| Symbol | Meaning | Verified against pinned IDF |
|---|---|---|
| `CONFIG_LOG_DEFAULT_LEVEL_WARN` | log level default | ✅ effective — `sdkconfig.h:617` sets `CONFIG_LOG_DEFAULT_LEVEL 2` (WARN) |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | main task stack | ✅ effective — `sdkconfig.h:447` = 8192 |
| `CONFIG_SPIRAM` | enable PSRAM | ✅ effective — `sdkconfig.h:397`; Kconfig default is `n` |
| `CONFIG_SPIRAM_SPEED_80M` | PSRAM 80 MHz | ✅ effective — `sdkconfig.h:400`; Kconfig default is **40 MHz** |
| `CONFIG_ESPTOOLPY_FLASHFREQ_80M` | flash 80 MHz | ✅ effective — `sdkconfig.h:264`; Kconfig default is 40 MHz |
| `CONFIG_ESPTOOLPY_FLASHSIZE_4MB` | flash size | ✅ effective — `sdkconfig.h:266`; Kconfig default is **2 MB** (source of the "Expected 4MB, found 2MB" warning) |

**Dependency discovered while verifying:** `CONFIG_SPIRAM_SPEED_80M` is
declared `depends on ESPTOOLPY_FLASHFREQ_80M`, so flash must be set to 80 MHz
first or the PSRAM option is silently unavailable. This is why both lines are
present. R2's README requires PSRAM + both at 80 MHz (SPEC §1A.3).

**Caveat recorded from Kconfig:** at 80+80 MHz with 4 MB (32 Mbit) PSRAM,
one of HSPI/VSPI is occupied by the system
(`SPIRAM_OCCUPY_SPI_HOST`). Application code must not touch those hosts. The
camera uses I2S and the SD card uses the SDMMC peripheral, so neither should
collide — but this must be re-checked if any future component claims HSPI/VSPI.

### Two build failures worth remembering

1. **`undefined reference to esp_psram_get_size`** — two distinct causes
   were hiding behind one symptom:
   - *Component not linked:* IDF auto-adds only its "common requirements"
     (esp_system, heap, freertos, log …) to every component. Because ours is
     named `src` rather than `main`, it gets **no** extra automatic
     dependencies, so `PRIV_REQUIRES esp_psram` is required in
     `firmware/src/CMakeLists.txt`. The PlatformIO ESP-IDF docs warn about
     exactly this when renaming `main`.
   - *Object never compiled:* `esp_psram.c` (which defines the symbol) is
     guarded by `if(CONFIG_SPIRAM)` in the component's CMakeLists, so with
     PSRAM disabled the header still declares the function but no object
     defines it — a header/library mismatch that looks like a naming problem.
   The symbol name itself was correct; SPEC §1A.10's "name varies by IDF
   version" warning did **not** apply here.

Camera-specific options (`CONFIG_CAMERA_*`, camera task core, DMA/JPEG buffer
mode) are added in the same verified way when the camera driver lands.

## Consequences

- Changing the platform or IDF version requires **a new ADR** and a re-run of
  the Phase 0 camera smoke test (per the `visioncam-build-flash` skill).
- Exactly one `esp32-camera` copy may be linked (rule 33); the configure-time
  check lives in the root `CMakeLists.txt` and both its pass and fail paths
  have been exercised (see above).
- **The partition table has two sources of truth** — `sdkconfig.defaults`
  (`CONFIG_PARTITION_TABLE_CUSTOM*`) and `platformio.ini`
  (`board_build.partitions`). Both must name `partitions.csv`. Changing one
  without the other produces a table that disagrees with the flashed app;
  re-run the byte-comparison check in "Partition table" after any change.
- The Arduino core is **not** present; every R1 Arduino API must be ported
  using SPEC §1A.10.

## Verification log

| Date | What | Result |
|---|---|---|
| 2026-11-15 | Platform + framework pins chosen from registry metadata | done |
| 2026-11-15 | `esp32cam` board resolves to 4 MB flash / 240 MHz | done (`pio boards esp32cam`) |
| 2026-11-15 | R2 2.1.9 supports classic ESP32, commit matches SPEC | done (registry page) |
| 2026-11-15 | First `pio run` completes; resolved IDF version read from build output | ✅ **6.1.0** — `framework-espidf @ 4.60100.0 (6.1.0)`, cross-checked against `framework-espidf/version.txt` |
| 2026-11-15 | Kconfig symbols above confirmed present in pinned IDF | ✅ all six confirmed in generated `sdkconfig.h` |
| 2026-11-15 | Exact `platform_packages` framework pin added (rule 24) and rebuilds clean | ✅ `platformio/framework-espidf@4.60100.0` |
| 2026-11-15 | Firmware size recorded | ✅ RAM 4.3 % (13,932 / 327,680 B), Flash 17.5 % (183,784 B); `firmware.bin` 184,208 B |
| 2026-11-15 | `idf_component.yml` resolves (task 0.2 step 7–8) | ✅ `managed_components/` created: `espressif__esp32-camera` 2.1.9, `espressif__mqtt` 1.1.0, transitive `espressif__esp_jpeg` 1.3.2; all three compile, build SUCCESS |
| 2026-11-15 | Registry path for the MQTT component | ✅ `espressif/mqtt` (API 200); `espressif/esp-mqtt` → 404 |
| 2026-11-15 | Rule 33 single-copy check, pass path | ✅ `count=1`, build SUCCESS |
| 2026-11-15 | Rule 33 single-copy check, fail path | ✅ injected duplicate in `components/vc_test_dup/` → build FAILED listing both paths; fixture removed, build green |
| 2026-11-15 | Only one `esp32-camera` archive in the build tree | ✅ `libespressif__esp32-camera.a`; no camera component in IDF 6.1.0 |
| 2026-11-15 | Camera + SD smoke firmware compiles and links (task 0.3 steps 11–13) | ✅ RAM 8.2 % (26,840 / 327,680 B), Flash 36.9 % (387,356 B); `firmware.bin` 387,792 B; `esp_camera_init` defined exactly once at `400de3b8` |
| 2026-11-15 | `partitions.csv` validates under IDF `gen_esp32part.py` | ✅ 6 partitions, no overlap, alignments valid, exact fit to `0x400000` |
| 2026-11-15 | **Partition table reaches the artifact `pio upload` flashes** | ✅ after adding `board_build.partitions = partitions.csv` — `partitions.bin` parses as our 6-partition table (was `nvs 24K / phy 4K / factory 1M`) |
| 2026-11-15 | Both partition pipelines produce the same bytes | ✅ IDF-side rule re-run by hand with its exact args, `cmp` identical to `partitions.bin` (md5 `7ca60f29f1fa15cf0b949d746387d5da`) |
| 2026-11-15 | App upload offset is the `ota_0` slot | ✅ `parttool --partition-boot-default` → `0x180000 0x20000`; size denominator now 1,572,864 B |
| 2026-11-15 | `ota_data_initial.bin` generated (needs an `ota` data partition) | ✅ 8,192 B at `0x320000` |
| _pending_ | Smoke firmware flashes and reports PSRAM/heap on hardware | **UNVERIFIED — no board attached** (`/dev/ttyUSB*` absent) |
