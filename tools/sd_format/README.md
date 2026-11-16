# tools/sd_format — one-shot SD card formatter

Rewrites a microSD card as **one MBR partition + FAT32 across the whole
card** — the layout VisionCam, R1 and any PC can all read.

## Why

The owner's 8 GB SanDisk card shipped with a bootable Linux Mint image
(FAT boot/EFI + ext4 root). FATFS can only mount FAT, so VisionCam saw a
5,216,256 B volume next to an 8,053,063,680 B CSD
(`docs/HARDWARE.md` §4.2). The owner instructed on 2026-11-16 that the card
be reformatted with a suitable file system and partition table.

VisionCam **never** formats a card itself — SPEC §2.3 keeps
`format_if_mount_failed = false` forever, and a mount failure is reported,
not "fixed". Formatting therefore lives in this standalone tool: flash it,
run `format`, confirm, flash the firmware back. It is never part of a
release build.

## What it writes

FatFs `f_mkfs` from the pinned ESP-IDF 6.1.0 (`FM_ANY`, `n_fat=2`, auto
cluster size; verified in the pinned `ff.c`):

| Layer | Result |
|---|---|
| Partition table | MBR, one entry, start LBA 63, type `0x0C` (FAT32 LBA), covering the whole card |
| Filesystem | FAT32, two FATs, auto cluster size (32 KB at 8 GB) |
| exFAT / GPT / superfloppy | never — `FF_FS_EXFAT=0`, `FF_LBA64=0`, no `FM_SFD` |

Clean-room (owner decision D1): **no R1 code** — the layout comes from
Espressif's own FatFs.

## Usage

```bash
pio run -d tools/sd_format -t upload        # board on /dev/ttyUSB0
pio device monitor -d tools/sd_format       # 115200 baud
```

Commands, one ASCII line each:

| command | effect |
|---|---|
| `info` | card CID/CSD (D5 identity), MBR partition table, FAT geometry vs CSD. Also runs once automatically at boot. |
| `format` | prints a destructive warning, then waits for the exact line `ERASE` (case-sensitive). Any other line cancels; nothing is written before the confirmation. |
| `help` | usage |

After confirmation the tool runs `f_mkfs`, remounts, prints the new MBR and
geometry, then performs a write/read/delete self-test and ends with a
verdict line:

```
sdformat: FORMAT OK — MBR + FAT32, <bytes> B usable, <ms> ms
```

## Safety

- Typed confirmation (`ERASE`) gates every write; the warning names what is
  destroyed (the Mint image) and that recovery means re-imaging on a PC.
- The tool is only in flash while you flash it; the firmware never links it.
- Evidence: `docs/evidence/phase0-sd-format.log`.
