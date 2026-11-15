# `tools/nvs_provision` — Phase 0 D2 helper (one-shot diagnostic tool)

Writes the two Wi-Fi credentials R1 reads from NVS so that R1
(`ESP32-CAM_MJPEG2SD`) can join the owner's LAN. That lets the **D2 R1
baseline** be measured over HTTP from one workstation instead of joining the
camera's AP from a second device. Procedure: `docs/HARDWARE.md` §3.7.

**This tool is not part of the firmware and never ships.** It is a
maintenance utility in `tools/` (SPEC §13 layout).

## What it does

Commands at 115200 baud, one ASCII line, TAB separated:

| Command | Effect |
|---|---|
| `set\t<ssid>\t<pass>` | write both keys and commit, then read them back |
| `show` | report key **lengths** only (values hidden) |
| `join` | try joining with the stored keys and print the raw `WIFI_REASON_*` |

Keys written: namespace `ESP-CAM_MJPEG` (R1's `APP_NAME`), keys `ST_SSID` /
`ST_Pass` — the Preferences entries R1 reads when its SD config has no SSID
(`prefs.cpp:216-223`).

## Clean-room rule (owner decision D1)

This tool contains **no R1 code**. It writes two documented NVS keys — the
machine equivalent of typing the credentials into R1's own setup page. Nothing
is copied, translated or linked from R1 (AGENTS rule 32).

## Security (AGENTS rule 17)

- The passphrase is read from NetworkManager and sent **only** to the UART by
  `/tmp/opencode/provision.py`. It is never printed, never logged, and never
  passed on a command line.
- This firmware prints **lengths only** and never echoes an unknown command
  line (it might contain a passphrase).
- Nothing credential-bearing is committed to this repository or into the tool
  image.

## Pins

Identical to the root project (`platformio.ini`, ADR-0001): PlatformIO
`espressif32@8.0.0`, `framework-espidf@4.60100.0`. The local `partitions.csv`
places `nvs` at `0x9000` with the same 20 K geometry arduino-esp32's
`PartitionScheme=min_spiffs` uses, so credentials written here are still
readable **after** R1 overwrites the partition table. Both partition
pipelines point at this one file (`sdkconfig.defaults` +
`board_build.partitions`) — the two-pipeline rule from ADR-0001.

## Build / flash

```bash
pio run -d tools/nvs_provision
pio run -d tools/nvs_provision -t upload
# board reboots into the tool; drive it with /tmp/opencode/provision.py
arduino-cli upload --fqbn esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=min_spiffs \
  --port /dev/ttyUSB0 /tmp/opencode/r1build/ESP32-CAM_MJPEG2SD
```

## Known behaviour notes

- `fgets`/`stdin` returns EOF immediately on this IDF configuration (the
  simple VFS read reports "no data" as end-of-file), so console input goes
  through the UART0 driver explicitly, one byte at a time.
- **Opening the serial port resets the board** (DTR/RTS pulse on the
  CH341/CH340 adapter), and R1 does **not** persist `/control` changes across
  reboots — re-apply `sdFreeSpaceMode=0` and `enableMotion=0` after every
  capture restart. Both facts were hit while measuring the baseline.