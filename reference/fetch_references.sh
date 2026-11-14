#!/usr/bin/env bash
# VisionCam Pro — verify or re-fetch the pinned reference checkouts.
#
#   reference/fetch_references.sh --verify     check the trees match pins.txt
#   reference/fetch_references.sh --fetch      clone upstream at pinned state
#   reference/fetch_references.sh --help
#
# Must be run from the repository root: the tree hash covers relative paths,
# so the working directory is part of the pin.

set -euo pipefail

R1_PATH="reference/ESP32-CAM_MJPEG2SD-master"
R2_PATH="reference/esp32-camera-2.1.9"
R1_URL="https://github.com/s60sc/ESP32-CAM_MJPEG2SD"
R2_URL="https://github.com/espressif/esp32-camera"
R1_SHA="20e532c74369188b719ab149bc9ecdae99f35c61a698d50a36c8029ef4701c0c"
R2_SHA="c9639edbd08a445dad325c46b96cbf7098fd3bf69fcea9b99711d14a14064d10"

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
note() { printf '%s\n' "$*"; }

tree_sha() { # $1 = directory relative to repo root
  [ -d "$1" ] || die "missing directory: $1"
  find "$1" -type f | LC_ALL=C sort | xargs sha256sum | sha256sum | cut -d' ' -f1
}

verify() {
  local ok=0
  local a b
  a="$(tree_sha "$R1_PATH")"
  b="$(tree_sha "$R2_PATH")"
  if [ "$a" = "$R1_SHA" ]; then note "OK   R1 $R1_PATH"; else
    note "FAIL R1 $R1_PATH"; note "     expected $R1_SHA"; note "     actual   $a"; ok=1
  fi
  if [ "$b" = "$R2_SHA" ]; then note "OK   R2 $R2_PATH"; else
    note "FAIL R2 $R2_PATH"; note "     expected $R2_SHA"; note "     actual   $b"; ok=1
  fi
  if [ "$ok" -ne 0 ]; then
    note ""
    note "A pin has moved. Before trusting any previously verified claim:"
    note "  1. re-read SPEC.md section 1A, and"
    note "  2. re-check .opencode/plans/00-analysis.md section 7.1 against the new tree."
    exit 1
  fi
  note "Both reference pins verified."
}

fetch() {
  command -v git >/dev/null || die "git is required"
  local tmp
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/visioncam-ref.XXXXXX")"
  trap 'rm -rf "$tmp"' EXIT

  note "Cloning R1 ($R1_URL) — AGPL-3.0, behavioural reference only..."
  git clone --quiet --depth 1 "$R1_URL" "$tmp/r1"
  rm -rf "$R1_PATH"
  mv "$tmp/r1" "$R1_PATH"
  rm -rf "$R1_PATH/.git"

  note "Cloning R2 ($R2_URL) — Apache-2.0, pinned runtime dependency..."
  git clone --quiet --depth 1 "$R2_URL" "$tmp/r2"
  rm -rf "$R2_PATH"
  mv "$tmp/r2" "$R2_PATH"
  rm -rf "$R2_PATH/.git"

  note ""
  note "Fetched. The trees will NOT match pins.txt unless upstream is unchanged"
  note "at HEAD — that is expected. If they differ:"
  note "  - update pins.txt with the new hashes, and"
  note "  - re-verify every claim in .opencode/plans/00-analysis.md section 7.1."
  note ""
  verify || note "(pin mismatch is expected after a fresh fetch — see above)"
}

usage() {
  sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'
}

case "${1:-}" in
  --verify) verify ;;
  --fetch)  fetch ;;
  --help|-h|*) usage ;;
esac
