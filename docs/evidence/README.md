# Hardware evidence logs

Raw serial captures from the Phase 0 smoke test on the owner's board
(2026-11-15), captured with `/tmp/opencode/serial_capture.py` at 115200 baud.

| File | What it proves |
|---|---|
| `phase0-smoke-boot-10-wifi-lamp.log` | steps 10–14: banner/PSRAM/heap, camera PID, SD 1-bit mount + self-test, GPIO4 drive-low, Wi-Fi off/on cycles (`step 14 result: OK`) |
| `phase0-smoke-boot-11-forced-retry.log` | the complete run with the final verdict line, including the forced first-attempt failure → `attempts=2 -> OK` recovery |
| `phase0-d2-r1-serial-run.txt` | the whole **D2 R1 baseline run**: R1 provisioned onto the LAN, driven by `curl`, 8 recordings closed, `checkMemory` after each, no brownout/panic/WDT |
| `phase0-d2-r1-avi-stats-blocks.txt` | the 8 verbatim AVI stats blocks behind every row of `docs/HARDWARE.md` §4 (FPS, SD write speed, busy %, heap) |
| `phase0-d2-vc-fat-crosscheck-boot.log` | VisionCam reflashed after the R1 run: smoke verdict line again, plus the independent FAT-volume measurement (5,216,256 B usable vs 8,053,063,680 B CSD) |

Provenance rules (AGENTS rule 26):

- These are **copies**; the originals live under `/tmp/opencode/smoke_boot_*.log`
  and `/tmp/opencode/r1_*.log` and are not durable.
- The only edit applied was sanitising stray binary bytes from the serial
  stream to printable ASCII/CR/LF (`tr -cd '\11\12\15\40-\176'`) so the files
  are text. No log line was added, removed or rewritten.
- Results from these files are transcribed into `docs/HARDWARE.md` §2/§4/§5.
  Nothing in `docs/` may cite evidence that is not in this directory.
