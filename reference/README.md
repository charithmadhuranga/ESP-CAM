# reference/ — pinned behavioural evidence

This directory holds **pinned, non-compiled checkouts** of the two upstream
projects VisionCam Pro is designed against.

## What is here

| Directory | Project | Licence | Role |
|---|---|---|---|
| `ESP32-CAM_MJPEG2SD-master/` | [s60sc/ESP32-CAM_MJPEG2SD](https://github.com/s60sc/ESP32-CAM_MJPEG2SD) (R1) | **AGPL-3.0** | Behavioural reference only |
| `esp32-camera-2.1.9/` | [espressif/esp32-camera](https://github.com/espressif/esp32-camera) (R2) | Apache-2.0 | The runtime camera driver (pinned dependency) |

Exact pins (tree hashes, file counts, verification commands) are in
[`pins.txt`](pins.txt). Re-verify with:

```bash
reference/fetch_references.sh --verify
```

The digest is computed over files sorted with `LC_ALL=C` — plain `sort` is
locale-dependent (`en_US.UTF-8` orders differently from `C`) and would produce
a different hash, so the script pins the locale explicitly.

## Hard rules

1. **R1 is AGPL-3.0 and is never vendored, copied or mechanically translated
   into VisionCam** (AGENTS rule 32; owner decision **D1 = Option A,
   clean-room**, recorded 2026-11-15). Ideas, architecture and measured
   numbers may be reused; source may not. R1 is never linked into a build.
2. **R2 is a pinned dependency, never forked** (rule 33). Exactly one copy of
   `esp32-camera` may ever be linked; a build-time check enforces this.
3. These directories are **excluded from the firmware build** and from
   release archives. They are kept in the repository as evidence so that
   SPEC claims can be re-checked against the exact source that was read.
4. `reference/` is **not** in `.gitignore` — the pins must travel with the
   repo — but it is never compiled (rule 5 of `.gitignore` comments).

## Why the tree hashes

Neither checkout carries `.git` metadata (both were unpacked from source
archives), so a commit SHA cannot be recorded. `pins.txt` therefore records a
SHA-256 over the sorted per-file hashes of each tree, which is stable and
re-checkable with `fetch_references.sh --verify`.

If either tree changes, the pin is broken: re-read `SPEC.md` §1A and the
affected sections of `.opencode/plans/00-analysis.md` §7.1 before trusting any
claim that was verified against the old tree.
