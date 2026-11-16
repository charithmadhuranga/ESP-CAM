# Hardware evidence logs

Raw serial captures from the Phase 0 smoke test on the owner's board,
captured with raw termios scripts at 115200 baud (2026-11-15 and
2026-11-16).

| File | What it proves |
|---|---|
| `phase0-smoke-boot-10-wifi-lamp.log` | steps 10–14: banner/PSRAM/heap, camera PID, SD 1-bit mount + self-test, GPIO4 drive-low, Wi-Fi off/on cycles (`step 14 result: OK`) |
| `phase0-smoke-boot-11-forced-retry.log` | the complete run with the final verdict line, including the forced first-attempt failure → `attempts=2 -> OK` recovery |
| `phase0-d2-r1-serial-run.txt` | the whole **D2 R1 baseline run**: R1 provisioned onto the LAN, driven by `curl`, 8 recordings closed, `checkMemory` after each, no brownout/panic/WDT |
| `phase0-d2-r1-avi-stats-blocks.txt` | the 8 verbatim AVI stats blocks behind every row of `docs/HARDWARE.md` §4 (FPS, SD write speed, busy %, heap) |
| `phase0-d2-vc-fat-crosscheck-boot.log` | VisionCam reflashed after the R1 run: smoke verdict line again, plus the independent FAT-volume measurement (5,216,256 B usable vs 8,053,063,680 B CSD) |
| `phase0-sd-format.log` | **the SD reformat run (2026-11-16):** `tools/sd_format/` auto-`info` at boot showing the pre-format Linux Mint partition table (`p2 0xEF` EFI 5,242,880 B + `p3 0x83` ext4 4,959,764,480 B, `FAT covers 0%` + warning), the typed-`ERASE`-gated `f_mkfs` (2,791 ms), the new MBR (`p1: 0x0C`, LBA 63, 8,053,031,424 B), FAT32 8,051,032,064 B usable (99 %), write/read/delete self-test, `FORMAT OK`, and post-format `info`/`help`. *Note: the first ~440 lines are the ESP32-CAM-MB's EN line chattering while the host released DTR/RTS (81 ROM banners); the verified session starts at the `POWERON_RESET` boot.* |
| `phase0-smoke-boot-12-postformat.log` | VisionCam reflashed **after** the reformat: `FAT volume: 8051032064 B usable … CSD capacity 8053063680 B` with **no** coverage warning, SD self-tests, full verdict line green (`camera: OK \| sd: OK \| psram: OK \| wifi(step14): OK \| forced-retry: OK`), single `POWERON_RESET`, no brownout/panic |
| `phase0-d2-r1-lamp-run.log` | **the GPIO4 lamp follow-up run (2026-11-16):** R1 flashed again over the reformatted card — SD mounts `SDHC, Size: 7.5GB, 1 bit mode` at `00:00:00.572`, R1's lamp feature initialises at `00:00:01.277` (`setupLamp: Mono LED available on pin 4`), Wi-Fi association fails (`status: 6`) so R1 falls back to AP + `Need to connect to AP to setup router`. The owner's visual observation (boot-window flash + repeating blink) is recorded in `docs/HARDWARE.md` §4; this log supplies the timestamps that separate the two events |

Provenance rules (AGENTS rule 26):

- These are **copies**; the originals live under `/tmp/opencode/` (`smoke_boot_*.log`,
  `r1_*.log`, `sdformat_raw.log`, `boot12.txt`) and are not durable.
- The only edit applied was sanitising stray binary bytes from the serial
  stream to printable ASCII/CR/LF (`tr -cd '\11\12\15\40-\176'`) so the files
  are text. No log line was added, removed or rewritten.
- Results from these files are transcribed into `docs/HARDWARE.md` §2/§4/§5.
  Nothing in `docs/` may cite evidence that is not in this directory.
