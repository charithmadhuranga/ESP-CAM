"""Narration timing engine.

Turns a narration markdown + its audio file into an exact per-beat schedule.

Method
------
There are no word-level timestamps in the source material, so each beat is
assigned the slice of narration it covers (a paragraph range) and receives a
share of the real audio duration proportional to how many characters of
narration it holds::

    beat_duration = audio_duration * (chars_in_beat / chars_in_script)

This is exact at the segment level by construction (the slices sum to the whole
script, so the durations sum to the audio duration) and tracks the narration
within a beat to the accuracy of a uniform speech rate. Speech rate is not
perfectly uniform -- measured across a typical multi-segment script it ranges
1.5-2.6 words/second -- so within-beat drift of a second or two is expected
on pauses.

That is fine for motion graphics, where beats run several seconds each, but it
is an estimate, not forced alignment. If tighter sync is ever needed, swap this
module for a forced aligner (e.g. aeneas or whisper word timestamps) -- the rest
of the pipeline consumes ``{"start", "duration"}`` and would not change.
"""

from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path
from typing import Any

HEADING_RE = re.compile(r"^#{1,6}\s*.*$", re.MULTILINE)


def probe_duration(path: Path) -> float:
    """Exact duration of a media file in seconds (via ffprobe)."""
    out = subprocess.run(
        [
            "ffprobe", "-v", "error",
            "-show_entries", "format=duration",
            "-of", "default=nw=1:nk=1",
            str(path),
        ],
        capture_output=True, text=True, check=True,
    ).stdout.strip()
    if not out:
        raise ValueError(f"ffprobe returned no duration for {path}")
    return float(out)


def read_paragraphs(md_path: Path) -> list[str]:
    """Split a narration markdown into ordered paragraphs.

    The leading ``# heading`` line is dropped; everything else is split on
    blank lines. Whitespace inside a paragraph is normalised to single spaces
    so character weighting is not distorted by line wrapping.
    """
    text = HEADING_RE.sub("", md_path.read_text(encoding="utf-8"))
    paras: list[str] = []
    for chunk in re.split(r"\n\s*\n", text):
        flat = " ".join(chunk.split())
        if flat:
            paras.append(flat)
    return paras


def allocate(paragraphs: list[str], total: float) -> list[dict[str, float]]:
    """Give each paragraph a start time and duration summing to ``total``."""
    if not paragraphs:
        raise ValueError("no paragraphs to schedule")
    weights = [max(1, len(p)) for p in paragraphs]
    weight_sum = sum(weights)

    out: list[dict[str, float]] = []
    cursor = 0.0
    for w in weights:
        dur = total * w / weight_sum
        out.append({"start": round(cursor, 4), "duration": round(dur, 4)})
        cursor += dur
    # Absorb float drift into the last beat so the timeline lands exactly on
    # the audio duration; a one-frame slip here would desync every later beat.
    drift = total - cursor
    out[-1]["duration"] = round(out[-1]["duration"] + drift, 4)
    return out


def _aligned_times(mp3_path: Path, paragraphs: list[str],
                   total: float) -> list[dict[str, float]] | None:
    """Paragraph times from ``video/align.py`` output, when present.

    ``<audio>.align.json`` holds the measured start of every paragraph
    (whisper word timestamps). Missing boundaries are filled by character
    weighting between their measured neighbours. Returns None when there is
    no alignment for this audio (or it is stale), so the caller falls back
    to pure character weighting.
    """
    path = mp3_path.with_suffix(".align.json")
    if not path.exists():
        return None
    data = json.loads(path.read_text(encoding="utf-8"))
    starts = data.get("para_starts") or []
    if len(starts) != len(paragraphs) or abs(
            data.get("audio_duration", 0) - total) > 0.05:
        return None
    bounds = list(starts) + [total]
    i = 0
    while i < len(bounds):
        if bounds[i] is None:
            j = i
            while bounds[j] is None:
                j += 1
            lo, hi = bounds[i - 1], bounds[j]
            w = [max(1, len(p)) for p in paragraphs[i - 1:j]]
            acc = 0
            for k in range(i, j):
                acc += w[k - i]
                bounds[k] = lo + (hi - lo) * acc / sum(w)
            i = j
        i += 1
    return [{"start": round(bounds[k], 4),
             "duration": round(bounds[k + 1] - bounds[k], 4)}
            for k in range(len(paragraphs))]


def build_schedule(segment: dict[str, Any], root: Path) -> dict[str, Any]:
    """Resolve one segment dict into a concrete, validated timeline.

    ``segment["beats"]`` entries carry ``paras: [start, end)`` (a half-open
    range of paragraph indices). This function checks those ranges tile the
    script exactly, then stamps ``start``/``duration`` onto every beat.
    """
    md_path = root / segment["script"]
    mp3_path = root / segment["audio"]
    paragraphs = read_paragraphs(md_path)
    audio_duration = probe_duration(mp3_path)
    para_times = allocate(paragraphs, audio_duration)
    aligned = _aligned_times(mp3_path, paragraphs, audio_duration)
    if aligned is not None:
        para_times = aligned

    beats = segment["beats"]
    cursor = 0
    for i, beat in enumerate(beats):
        start, end = beat["paras"]
        if start != cursor:
            raise ValueError(
                f"{segment['id']} beat {i}: expected paragraph {cursor}, "
                f"got {start} — beats must tile the script without gaps"
            )
        if end <= start:
            raise ValueError(f"{segment['id']} beat {i}: empty paragraph range")
        if end > len(paragraphs):
            raise ValueError(
                f"{segment['id']} beat {i}: range {start}:{end} exceeds "
                f"{len(paragraphs)} paragraphs"
            )
        cursor = end

        beat_start = para_times[start]["start"]
        beat_end = para_times[end - 1]["start"] + para_times[end - 1]["duration"]
        beat["start"] = round(beat_start, 4)
        beat["duration"] = round(beat_end - beat_start, 4)
        beat["narration"] = " ".join(paragraphs[start:end])

    if cursor != len(paragraphs):
        raise ValueError(
            f"{segment['id']}: beats cover {cursor} of {len(paragraphs)} "
            "paragraphs — the tail of the script has no visual"
        )

    return {
        "id": segment["id"],
        "title": segment["title"],
        "audio": str(mp3_path),
        "paragraphs": len(paragraphs),
        "audio_duration": round(audio_duration, 4),
        "beats": [
            {**b, "i": i}
            for i, b in enumerate(beats)
        ],
    }


def build_all(segments: list[dict[str, Any]], root: Path) -> dict[str, Any]:
    """Build and validate every segment schedule; writes nothing."""
    schedules = [build_schedule(s, root) for s in segments]
    return {
        "segments": schedules,
        "total_audio": round(sum(s["audio_duration"] for s in schedules), 3),
    }


def write_schedule(data: dict[str, Any], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")
