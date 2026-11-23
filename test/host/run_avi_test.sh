#!/usr/bin/env bash
# Host test: build avi_writer, assemble an AVI from synthetic JPEGs (odd
# and even lengths), then require ffprobe to see one MJPEG stream with the
# right frame count and ffmpeg to decode every frame without error.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

g++ -std=c++17 -Wall -Wextra -Werror -I "$root/firmware/src/recording" \
    "$root/test/host/avi_writer_test.cpp" \
    "$root/firmware/src/recording/avi_writer.cpp" -o "$work/avi_test"

n=12
for i in $(seq 1 $n); do
  ffmpeg -loglevel error -f lavfi -i "testsrc=size=640x480:rate=1" \
         -frames:v 1 -q:v $((2 + i % 7)) "$work/f$i.jpg"
done
"$work/avi_test" "$work/out.avi" 640 480 $(for i in $(seq 1 $n); do echo "$work/f$i.jpg"; done)

probe=$(ffprobe -v error -count_frames -select_streams v:0 \
  -show_entries stream=codec_name,width,height,nb_read_frames,avg_frame_rate \
  -of default=nw=1 "$work/out.avi")
echo "$probe"
grep -q 'codec_name=mjpeg' <<<"$probe"
grep -q 'width=640' <<<"$probe"
grep -q 'height=480' <<<"$probe"
grep -q "nb_read_frames=$n" <<<"$probe"
grep -q 'avg_frame_rate=10/1' <<<"$probe"
errs=$(ffmpeg -v error -i "$work/out.avi" -f null - 2>&1 || true)
[ -z "$errs" ] || { echo "decode errors: $errs"; exit 1; }
odd=$(for i in $(seq 1 $n); do s=$(stat -c %s "$work/f$i.jpg"); echo $((s % 2)); done | sort -u | tr -d '\n')
echo "JPEG length parities covered: $odd"
echo "PASS"
