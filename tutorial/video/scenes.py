"""One Manim scene class per narration segment (see build.py scene_name)."""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from beats import SegmentScene  # noqa: E402


class Intro(SegmentScene):
    segment_id = "intro"


class Ch01(SegmentScene):
    segment_id = "ch1"


class Ch02(SegmentScene):
    segment_id = "ch2"


class Ch03(SegmentScene):
    segment_id = "ch3"


class Ch04(SegmentScene):
    segment_id = "ch4"


class Ch05(SegmentScene):
    segment_id = "ch5"


class End(SegmentScene):
    segment_id = "end"
