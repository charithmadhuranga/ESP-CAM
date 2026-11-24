"""Paragraph-level forced alignment from faster-whisper word timestamps.

For every segment, transcribe its mp3 with word timestamps, align the
transcript words to the script words (difflib), and record the audio time at
which each script paragraph starts. ``timing.build_schedule`` uses these
boundaries when ``<audio>.align.json`` exists, so a beat changes when the
narrator actually starts its paragraph instead of at a character-weighted
estimate.

    .venv/bin/python video/align.py            # all segments
    .venv/bin/python video/align.py ch3        # one segment
"""
from __future__ import annotations

import json
import re
import sys
from difflib import SequenceMatcher
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

from segments import ORDER, SEGMENTS  # noqa: E402
from timing import read_paragraphs, probe_duration  # noqa: E402

PUNCT = re.compile(r"[^\w\s]")


def norm(w: str) -> str:
    return PUNCT.sub("", w.lower().replace("'", "")).strip()


def align(seg_id: str, model) -> dict:
    seg = SEGMENTS[seg_id]
    paras = read_paragraphs(ROOT / seg["script"])
    audio = ROOT / seg["audio"]
    script_words, owner = [], []
    for i, p in enumerate(paras):
        for w in p.split():
            n = norm(w)
            if n:
                script_words.append(n)
                owner.append(i)
    segments, _ = model.transcribe(str(audio), word_timestamps=True,
                                   language="en", beam_size=5)
    asr = [(norm(w.word), w.start, w.end)
           for s in segments for w in (s.words or []) if norm(w.word)]
    sm = SequenceMatcher(a=script_words, b=[a[0] for a in asr],
                         autojunk=False)
    # script word index -> audio start time
    t_of: dict[int, float] = {}
    for a0, b0, n in sm.get_matching_blocks():
        for k in range(n):
            t_of[a0 + k] = asr[b0 + k][1]
    starts = []
    for i in range(len(paras)):
        idx = [j for j, o in enumerate(owner) if o == i]
        # first matched word of the paragraph; backfill by up to 3 words
        hit = next(((j, t_of[j]) for j in idx[:6] if j in t_of), None)
        if hit is None:
            starts.append(None)
            continue
        j, t = hit
        # back off for unmatched leading words (~0.3 s each)
        starts.append(float(max(0.0, t - 0.3 * (j - idx[0]))))
    dur = probe_duration(audio)
    # boundary = slightly before the paragraph's first word (inside the pause)
    bounds = [0.0]
    for i in range(1, len(paras)):
        s = starts[i]
        if s is None:
            bounds.append(None)
        else:
            bounds.append(round(max(bounds[-1] or 0, s - 0.25), 3))
    out = {"audio_duration": dur, "para_starts": bounds,
           "matched_words": sum(n for *_, n in sm.get_matching_blocks()),
           "script_words": len(script_words)}
    return out


def main() -> None:
    from faster_whisper import WhisperModel
    model = WhisperModel("small", device="cpu", compute_type="int8")
    ids = sys.argv[1:] or ORDER
    for sid in ids:
        res = align(sid, model)
        path = (ROOT / SEGMENTS[sid]["audio"]).with_suffix(".align.json")
        path.write_text(json.dumps(res, indent=2))
        print(f"{sid:<6} {res['matched_words']}/{res['script_words']} words "
              f"matched  starts={res['para_starts']}")


if __name__ == "__main__":
    main()
