"""Manim scene framework for the VisionCam Pro tutorial series.

A ``SegmentScene`` plays one narration segment. Each beat in ``segments.py``
claims a paragraph range; ``timing.build_schedule`` turns that into a start
and a duration measured from the real recording. Playback is locked to that
absolute timeline: every beat ends exactly at ``lead_in + start + duration``
whatever its animations cost, so frame rounding never accumulates drift.

Beat types (``_r_<type>`` builds, ``_play_beat`` paces):

    title, statement, bullets, pipeline, grid, code, chart, stack, stat,
    stats, flow, wiring, board, queue, avi, memory, motion, hacard,
    screenshot, cta, endscreen

Rules from the skill: entrances stagger and fit in about a third of the
beat; the rest is a hold, which some beats fill with motion (pulses along
edges, a queue dropping frames, a bell swinging). No progress bar.
"""

from __future__ import annotations

import math
import random
import sys
import textwrap
from pathlib import Path
from typing import Any, Callable

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import theme as T  # noqa: E402
from diagrams import DIAGRAMS  # noqa: E402
from segments import SEGMENTS  # noqa: E402
from timing import build_schedule  # noqa: E402

from manim import (  # noqa: E402
    BOLD, DOWN, LEFT, ORIGIN, RIGHT, UP, PI, AnimationGroup, Arc, Circle,
    Create, DashedLine, Dot, Ellipse, FadeIn, FadeOut, Group, GrowFromEdge,
    ImageMobject, LaggedStart, Line, MoveAlongPath, Polygon, Rectangle,
    Rotate, RoundedRectangle, Scene, Square, Succession, Text,
    UpdateFromAlphaFunc, VGroup, VMobject, Wait, AddTextLetterByLetter,
    Transform, Indicate, there_and_back, rate_functions,
)

ROOT = Path(__file__).resolve().parent.parent
IMAGES = ROOT.parent / "docs" / "images"

HEADER_Y = 3.55
CONTENT_TOP = 2.85
CONTENT_BOTTOM = -3.3
HALF_W = 7.11


# ==========================================================================
# text helpers
# ==========================================================================
def _wrap(text: str, chars: int) -> str:
    return "\n".join(textwrap.wrap(text, width=chars, break_long_words=False,
                                   break_on_hyphens=False))


def _char_width(size: int, *, bold: bool = False, mono: bool = False) -> float:
    if mono:
        return 0.0077 * size
    return (0.0085 if bold else 0.0074) * size


def _text(s: str, size: int = 32, color: str = T.TEXT, *, bold: bool = False,
          mono: bool = False, align: str = "center",
          max_width: float = 12.8) -> Text:
    if s and "\n" not in s:
        limit = max(8, int(max_width / _char_width(size, bold=bold, mono=mono)))
        if len(s) > limit:
            s = _wrap(s, limit)
    kw: dict[str, Any] = {"weight": BOLD} if bold else {}
    t = Text(s, font=T.FONT_MONO if mono else T.FONT, font_size=size,
             color=color, line_spacing=1.1, disable_ligatures=True, **kw)
    if align == "center" and "\n" in s:
        _center_lines(t, s.count("\n") + 1)
    return t


def _center_lines(t: Text, n_lines: int) -> None:
    glyphs = list(t.submobjects)
    if n_lines < 2 or not glyphs:
        return
    glyphs.sort(key=lambda g: -g.get_center()[1])
    ys = [g.get_center()[1] for g in glyphs]
    gaps = sorted(((ys[i] - ys[i + 1], i) for i in range(len(ys) - 1)),
                  reverse=True)
    splits = sorted(i for _, i in gaps[: n_lines - 1])
    lines, start = [], 0
    for cut in splits:
        lines.append(glyphs[start:cut + 1])
        start = cut + 1
    lines.append(glyphs[start:])
    for line in lines:
        if line:
            left = min(g.get_left()[0] for g in line)
            right = max(g.get_right()[0] for g in line)
            for g in line:
                g.shift(-(left + right) / 2 * RIGHT)


def _fit(m, w: float, h: float | None = None):
    s = 1.0
    if m.width > w:
        s = w / m.width
    if h is not None and m.height * s > h:
        s = h / m.height
    if s < 1.0:
        m.scale(s)
    return m


def _card(w: float, h: float, fill: str = T.CARD, stroke: str = T.BORDER,
          radius: float = 0.14, sw: float = 1.5) -> RoundedRectangle:
    return RoundedRectangle(corner_radius=radius, width=w, height=h,
                            fill_color=fill, fill_opacity=1.0,
                            stroke_color=stroke, stroke_width=sw)


def _heading(text: str, color: str = T.ACCENT) -> Text:
    return _text(text.upper(), 28, color, bold=True)


def _pill(text: str, color: str = T.ACCENT, size: int = 20,
          fill: str = T.PANEL) -> VGroup:
    t = _text(text, size, color, bold=True)
    box = RoundedRectangle(corner_radius=min(0.2, (t.height + 0.22) / 2),
                           width=t.width + 0.44, height=t.height + 0.24,
                           fill_color=fill, fill_opacity=1, stroke_color=color,
                           stroke_width=1.5)
    t.move_to(box)
    return VGroup(box, t)


def _poly(*pts, color=T.TEXT, width=4, closed=False, fill=None) -> VMobject:
    pts = [np.array([p[0], p[1], 0.0]) for p in pts]
    if closed:
        m = Polygon(*pts, stroke_color=color, stroke_width=width)
        if fill:
            m.set_fill(fill, 1)
        return m
    m = VMobject(stroke_color=color, stroke_width=width)
    m.set_points_as_corners(pts)
    return m


def _counter(value: float, decimals: int, size: int, color: str,
             suffix: str = "") -> tuple[VGroup, Callable]:
    """A number that counts up from 0 (no LaTeX needed)."""
    def fmt(v: float) -> str:
        return f"{v:.{decimals}f}{suffix}"
    holder = VGroup(_text(fmt(value), size, color, bold=True))
    final_center = None

    def anim(rt: float):
        nonlocal final_center
        final_center = holder.get_center()

        def upd(m, a):
            v = value * rate_functions.ease_out_cubic(a)
            new = _text(fmt(v), size, color, bold=True)
            new.move_to(final_center)
            m.submobjects[0].become(new)
        return UpdateFromAlphaFunc(holder, upd, run_time=rt)
    return holder, anim


# ==========================================================================
# icons: drawn from primitives so they render crisply without emoji fonts
# ==========================================================================
def icon(name: str, s: float = 0.6, color: str = T.ACCENT) -> VGroup:
    w = 3.2 * s
    g = VGroup()
    if name == "camera":
        body = RoundedRectangle(corner_radius=0.12 * s, width=s * 1.1,
                                height=s * 0.75, stroke_color=color,
                                stroke_width=w)
        bump = _poly((-0.2 * s, 0.375 * s), (-0.12 * s, 0.5 * s),
                     (0.12 * s, 0.5 * s), (0.2 * s, 0.375 * s),
                     color=color, width=w)
        lens = Circle(radius=0.22 * s, stroke_color=color, stroke_width=w)
        g.add(body, bump, lens)
    elif name == "eye":
        top = Arc(radius=0.75 * s, start_angle=PI / 2 - 0.75, angle=1.5,
                  stroke_color=color, stroke_width=w).shift(DOWN * 0.48 * s)
        bot = top.copy().rotate(PI, about_point=ORIGIN)
        pupil = Circle(radius=0.17 * s, fill_color=color, fill_opacity=1,
                       stroke_width=0)
        g.add(top, bot, pupil)
    elif name == "broadcast":
        g.add(Dot(radius=0.09 * s, color=color))
        for r in (0.28, 0.48):
            for sgn in (1, -1):
                a = Arc(radius=r * s, start_angle=-0.7 if sgn > 0 else PI - 0.7,
                        angle=1.4, stroke_color=color, stroke_width=w)
                g.add(a)
    elif name == "play":
        g.add(Circle(radius=0.48 * s, stroke_color=color, stroke_width=w))
        g.add(_poly((-0.13 * s, 0.22 * s), (0.22 * s, 0), (-0.13 * s, -0.22 * s),
                    closed=True, color=color, width=w, fill=color))
    elif name == "motion":
        g.add(Dot(radius=0.1 * s, color=color).shift(LEFT * 0.3 * s))
        for i, r in enumerate((0.22, 0.38, 0.54)):
            g.add(Arc(radius=r * s, start_angle=-0.8, angle=1.6,
                      arc_center=LEFT * 0.3 * s, stroke_color=color,
                      stroke_width=w, stroke_opacity=1 - i * 0.25))
    elif name == "home":
        g.add(_poly((-0.45 * s, -0.42 * s), (-0.45 * s, 0.08 * s),
                    (0, 0.48 * s), (0.45 * s, 0.08 * s), (0.45 * s, -0.42 * s),
                    closed=True, color=color, width=w))
        g.add(_poly((-0.12 * s, -0.42 * s), (-0.12 * s, -0.1 * s),
                    (0.12 * s, -0.1 * s), (0.12 * s, -0.42 * s),
                    color=color, width=w))
    elif name == "sd":
        g.add(_poly((-0.32 * s, -0.48 * s), (-0.32 * s, 0.3 * s),
                    (-0.14 * s, 0.48 * s), (0.32 * s, 0.48 * s),
                    (0.32 * s, -0.48 * s), closed=True, color=color, width=w))
        for i in range(4):
            x = -0.15 * s + i * 0.12 * s
            g.add(Line([x, 0.18 * s, 0], [x, 0.36 * s, 0], stroke_color=color,
                       stroke_width=w * 0.8))
    elif name == "chip":
        g.add(RoundedRectangle(corner_radius=0.06 * s, width=0.6 * s,
                               height=0.6 * s, stroke_color=color,
                               stroke_width=w))
        for i in (-0.15, 0, 0.15):
            for a, b in (((i, 0.3), (i, 0.48)), ((i, -0.3), (i, -0.48)),
                         ((0.3, i), (0.48, i)), ((-0.3, i), (-0.48, i))):
                g.add(Line([a[0] * s, a[1] * s, 0], [b[0] * s, b[1] * s, 0],
                           stroke_color=color, stroke_width=w * 0.8))
    elif name == "code":
        g.add(_text("</>", int(60 * s), color, bold=True, mono=True))
    elif name == "cross":
        g.add(Circle(radius=0.48 * s, stroke_color=T.DANGER, stroke_width=w))
        g.add(Line([-0.2 * s, -0.2 * s, 0], [0.2 * s, 0.2 * s, 0],
                   stroke_color=T.DANGER, stroke_width=w))
        g.add(Line([-0.2 * s, 0.2 * s, 0], [0.2 * s, -0.2 * s, 0],
                   stroke_color=T.DANGER, stroke_width=w))
    elif name == "lock":
        g.add(RoundedRectangle(corner_radius=0.08 * s, width=0.8 * s,
                               height=0.58 * s, stroke_color=color,
                               stroke_width=w).shift(DOWN * 0.18 * s))
        g.add(Arc(radius=0.24 * s, start_angle=0, angle=PI,
                  arc_center=UP * 0.11 * s, stroke_color=color,
                  stroke_width=w))
        g.add(Line(UP * 0.11 * s + LEFT * 0.24 * s, LEFT * 0.24 * s * 1,
                   stroke_color=color, stroke_width=w).shift(UP * 0.0))
        g.add(Line(UP * 0.11 * s + RIGHT * 0.24 * s, RIGHT * 0.24 * s,
                   stroke_color=color, stroke_width=w))
        g.add(Dot(radius=0.07 * s, color=color).shift(DOWN * 0.18 * s))
    elif name == "user":
        g.add(Circle(radius=0.19 * s, stroke_color=color,
                     stroke_width=w).shift(UP * 0.2 * s))
        g.add(Arc(radius=0.36 * s, start_angle=0, angle=PI,
                  arc_center=DOWN * 0.42 * s, stroke_color=color,
                  stroke_width=w))
    elif name == "share":
        pts = [np.array([-0.3, 0, 0]) * s, np.array([0.3, 0.32, 0]) * s,
               np.array([0.3, -0.32, 0]) * s]
        g.add(Line(pts[0], pts[1], stroke_color=color, stroke_width=w))
        g.add(Line(pts[0], pts[2], stroke_color=color, stroke_width=w))
        for p in pts:
            g.add(Dot(p, radius=0.11 * s, color=color))
    elif name == "chart":
        for i, h in enumerate((0.4, 0.7, 0.55)):
            g.add(Rectangle(width=0.2 * s, height=h * s, fill_color=color,
                            fill_opacity=1, stroke_width=0)
                  .move_to([(-0.28 + 0.28 * i) * s, (-0.45 + h / 2) * s, 0]))
    elif name == "wifi":
        g.add(Dot(radius=0.08 * s, color=color).shift(DOWN * 0.3 * s))
        for r in (0.25, 0.45, 0.65):
            g.add(Arc(radius=r * s, start_angle=PI / 4, angle=PI / 2,
                      arc_center=DOWN * 0.3 * s, stroke_color=color,
                      stroke_width=w))
    elif name == "button":
        g.add(RoundedRectangle(corner_radius=0.15 * s, width=0.95 * s,
                               height=0.6 * s, stroke_color=color,
                               stroke_width=w))
        g.add(Circle(radius=0.12 * s, fill_color=color, fill_opacity=1,
                     stroke_width=0))
    elif name == "bolt":
        g.add(_poly((0.08 * s, 0.5 * s), (-0.25 * s, -0.02 * s),
                    (0.0, -0.02 * s), (-0.08 * s, -0.5 * s),
                    (0.25 * s, 0.05 * s), (0.0, 0.05 * s), closed=True,
                    color=color, width=w * 0.7, fill=color))
    elif name == "clock":
        g.add(Circle(radius=0.46 * s, stroke_color=color, stroke_width=w))
        g.add(Line(ORIGIN, UP * 0.3 * s, stroke_color=color, stroke_width=w))
        g.add(Line(ORIGIN, RIGHT * 0.22 * s, stroke_color=color,
                   stroke_width=w))
    elif name == "gear":
        g.add(Circle(radius=0.3 * s, stroke_color=color, stroke_width=w))
        g.add(Circle(radius=0.11 * s, stroke_color=color, stroke_width=w))
        for k in range(8):
            a = k * PI / 4
            g.add(Line([0.3 * s * math.cos(a), 0.3 * s * math.sin(a), 0],
                       [0.47 * s * math.cos(a), 0.47 * s * math.sin(a), 0],
                       stroke_color=color, stroke_width=w * 1.6))
    elif name == "globe":
        g.add(Circle(radius=0.46 * s, stroke_color=color, stroke_width=w))
        g.add(Ellipse(width=0.42 * s, height=0.92 * s, stroke_color=color,
                      stroke_width=w * 0.8))
        g.add(Line(LEFT * 0.46 * s, RIGHT * 0.46 * s, stroke_color=color,
                   stroke_width=w * 0.8))
    elif name == "cloud":
        parts = [Circle(radius=0.22 * s).shift(LEFT * 0.22 * s + DOWN * 0.05 * s),
                 Circle(radius=0.3 * s).shift(UP * 0.08 * s + RIGHT * 0.05 * s),
                 Circle(radius=0.2 * s).shift(RIGHT * 0.32 * s + DOWN * 0.07 * s)]
        for p in parts:
            p.set_stroke(color, w)
        g.add(*parts)
    elif name == "shield":
        g.add(_poly((0, 0.5 * s), (0.42 * s, 0.32 * s), (0.36 * s, -0.18 * s),
                    (0, -0.5 * s), (-0.36 * s, -0.18 * s), (-0.42 * s, 0.32 * s),
                    closed=True, color=color, width=w))
        g.add(_poly((-0.16 * s, 0), (-0.03 * s, -0.14 * s), (0.18 * s, 0.14 * s),
                    color=color, width=w))
    elif name == "license":
        g.add(RoundedRectangle(corner_radius=0.05 * s, width=0.72 * s,
                               height=0.95 * s, stroke_color=color,
                               stroke_width=w))
        g.add(_text("MIT", int(26 * s), color, bold=True))
    elif name == "download":
        g.add(Line(UP * 0.42 * s, DOWN * 0.1 * s, stroke_color=color,
                   stroke_width=w))
        g.add(_poly((-0.2 * s, 0.1 * s), (0, -0.12 * s), (0.2 * s, 0.1 * s),
                    color=color, width=w))
        g.add(_poly((-0.42 * s, -0.18 * s), (-0.42 * s, -0.42 * s),
                    (0.42 * s, -0.42 * s), (0.42 * s, -0.18 * s),
                    color=color, width=w))
    elif name == "book":
        g.add(_poly((0, -0.38 * s), (-0.48 * s, -0.3 * s), (-0.48 * s, 0.38 * s),
                    (0, 0.3 * s), closed=True, color=color, width=w))
        g.add(_poly((0, -0.38 * s), (0.48 * s, -0.3 * s), (0.48 * s, 0.38 * s),
                    (0, 0.3 * s), closed=True, color=color, width=w))
    elif name == "check":
        g.add(_poly((-0.3 * s, 0), (-0.08 * s, -0.24 * s), (0.34 * s, 0.26 * s),
                    color=color, width=w * 1.3))
    else:
        raise ValueError(f"unknown icon {name!r}")
    return g


def thumbs_up(s: float = 1.0, color: str = T.ACCENT) -> VGroup:
    """A thumbs-up glyph built from rounded shapes."""
    cuff = RoundedRectangle(corner_radius=0.06 * s, width=0.42 * s,
                            height=1.15 * s, fill_color=color, fill_opacity=1,
                            stroke_width=0).move_to([-0.78 * s, -0.12 * s, 0])
    palm = RoundedRectangle(corner_radius=0.22 * s, width=1.15 * s,
                            height=1.15 * s, fill_color=color, fill_opacity=1,
                            stroke_width=0).move_to([0.05 * s, -0.12 * s, 0])
    thumb = RoundedRectangle(corner_radius=0.17 * s, width=0.38 * s,
                             height=0.95 * s, fill_color=color, fill_opacity=1,
                             stroke_width=0)
    thumb.rotate(-0.38).move_to([-0.12 * s, 0.68 * s, 0])
    lines = VGroup(*[Line([0.12 * s, y * s, 0], [0.62 * s, y * s, 0],
                          stroke_color=T.BG, stroke_width=4 * s)
                     for y in (0.18, -0.12, -0.42)])
    return VGroup(cuff, palm, thumb, lines)


def bell(s: float = 1.0, color: str = T.TEXT) -> VGroup:
    body = _poly((-0.42 * s, -0.3 * s), (-0.32 * s, -0.2 * s),
                 (-0.3 * s, 0.18 * s), (-0.12 * s, 0.42 * s),
                 (0.12 * s, 0.42 * s), (0.3 * s, 0.18 * s),
                 (0.32 * s, -0.2 * s), (0.42 * s, -0.3 * s), closed=True,
                 color=color, width=5 * s, fill=color)
    clapper = Dot([0, -0.42 * s, 0], radius=0.1 * s, color=color)
    knob = Dot([0, 0.5 * s, 0], radius=0.07 * s, color=color)
    return VGroup(body, clapper, knob)


def logo(s: float = 1.0) -> VGroup:
    """The VisionCam Pro mark (docs/images/logo-icon.svg) in native shapes."""
    sq = RoundedRectangle(corner_radius=0.44 * s, width=1.88 * s,
                          height=1.88 * s, stroke_width=0, fill_opacity=1)
    sq.set_fill(color=["#6aaefc", "#2f6fe0"], opacity=1)
    sq.set_sheen_direction(DOWN + RIGHT)
    off = DOWN * 0.03 * s
    housing = Circle(radius=0.59 * s, fill_color="#0f1115", fill_opacity=0.95,
                     stroke_color=T.TEXT, stroke_width=6 * s).shift(off)
    glass = Circle(radius=0.39 * s, fill_color="#26324a", fill_opacity=1,
                   stroke_color=T.ACCENT, stroke_width=4 * s).shift(off)
    pupil = Circle(radius=0.17 * s, fill_color="#0b0d11", fill_opacity=1,
                   stroke_color="#6aaefc", stroke_width=2.5 * s).shift(off)
    hi = Ellipse(width=0.22 * s, height=0.14 * s, fill_color=T.TEXT,
                 fill_opacity=0.75, stroke_width=0).rotate(0.6)
    hi.move_to([-0.14 * s, 0.13 * s, 0])
    led = Circle(radius=0.11 * s, fill_color="#2ecc71", fill_opacity=1,
                 stroke_color=T.TEXT, stroke_width=2.5 * s)
    led.move_to([0.55 * s, 0.6 * s, 0])
    return VGroup(sq, housing, glass, pupil, hi, led)


# ==========================================================================
# the scene
# ==========================================================================
class SegmentScene(Scene):
    segment_id: str = ""

    # -- lifecycle --------------------------------------------------------
    def construct(self) -> None:
        seg = SEGMENTS[self.segment_id]
        self.seg = seg
        self.camera.background_color = T.BG
        self._schedule = build_schedule(seg, ROOT)
        self._lead = float(seg.get("lead_in", 0.0))
        self._flow_state: dict[str, Any] = {}
        self._hold: list[Callable[[float], float]] = []
        self._post: list = []
        self._strays: list = []
        rnd = random.Random(7)
        self._bg_dots = VGroup(*[
            Dot([rnd.uniform(-7, 7), rnd.uniform(-3.4, 3), 0], radius=0.012,
                color=T.BORDER) for _ in range(110)])
        self.add(self._bg_dots)

        if self._lead > 0:
            self._play_lead_in(seg)
        else:
            self.add(self._header(seg))

        prev = None
        for beat in self._schedule["beats"]:
            prev = self._run_beat(beat, prev)

        tail = float(seg.get("tail_hold", 0.0))
        if tail > 0:
            self.wait(tail)
        top = [m for m in self.mobjects if m is not self._bg_dots]
        self.play(*[FadeOut(m) for m in top], run_time=0.6)

    def _now(self) -> float:
        return self.time - self._lead

    def _wait_until(self, t: float) -> None:
        dt = t - self._now()
        if dt > 1 / 60:
            self.wait(dt)

    # -- chrome -----------------------------------------------------------
    def _header(self, seg) -> VGroup:
        num = seg.get("number")
        badge_text = f"CH {num:02d}" if num is not None else seg["id"].upper()
        badge = _text(badge_text, 22, T.BG, bold=True)
        box = _card(badge.width + 0.42, 0.5, fill=T.ACCENT, stroke=T.ACCENT,
                    radius=0.12)
        box.move_to([-(HALF_W - 0.6 - box.width / 2), HEADER_Y, 0])
        badge.move_to(box)
        title = _text(seg["title"], 24, T.TEXT_DIM)
        title.move_to([box.get_right()[0] + 0.3 + title.width / 2, HEADER_Y, 0])
        brand = _text("VisionCam Pro", 22, T.TEXT_MUTED, bold=True)
        mark = logo(0.2)
        bg = VGroup(mark, brand).arrange(RIGHT, buff=0.16)
        bg.move_to([HALF_W - 0.6 - bg.width / 2, HEADER_Y, 0])
        rule = Line([-(HALF_W - 0.6), HEADER_Y - 0.42, 0],
                    [HALF_W - 0.6, HEADER_Y - 0.42, 0],
                    stroke_color=T.BORDER, stroke_width=1.5)
        return VGroup(box, badge, title, bg, rule)

    def _play_lead_in(self, seg) -> None:
        L = self._lead
        if seg["id"] == "intro":
            mark = logo(1.25)
            name = _text("PROTO VISION LAB", 30, T.TEXT_MUTED, bold=True)
            pres = _text("presents", 22, T.TEXT_MUTED)
            grp = VGroup(mark, name, pres).arrange(DOWN, buff=0.35)
            grp.move_to([0, 0.2, 0])
            sq, housing, glass, pupil, hi, led = mark
            self.play(LaggedStart(
                FadeIn(sq, scale=0.6), Create(housing), Create(glass),
                FadeIn(pupil, scale=0.5), FadeIn(hi), FadeIn(led, scale=2),
                lag_ratio=0.18), run_time=1.3)
            self.play(FadeIn(name, shift=UP * 0.2), FadeIn(pres), run_time=0.5)
            self.play(Indicate(led, color="#2ecc71", scale_factor=1.6),
                      run_time=0.5)
            self.wait(max(0.0, L - 0.6 - 2.3))
            hdr = self._header(seg)
            self.play(FadeOut(grp, scale=0.9), FadeIn(hdr), run_time=0.6)
            return
        num = seg.get("number", 0)
        eyebrow = _text(f"CHAPTER {num:02d}", 26, T.ACCENT, bold=True)
        title = _text(seg["title"], 60, T.TEXT, bold=True, max_width=12)
        rule = Line(LEFT * 1.4, RIGHT * 1.4, stroke_color=T.ACCENT,
                    stroke_width=5)
        grp = VGroup(eyebrow, title, rule).arrange(DOWN, buff=0.34)
        grp.move_to([0, 0.15, 0])
        self.play(LaggedStart(FadeIn(eyebrow, shift=UP * 0.3),
                              FadeIn(title, shift=UP * 0.3), Create(rule),
                              lag_ratio=0.25), run_time=0.9)
        self.wait(max(0.0, L - 0.9 - 0.5))
        hdr = self._header(seg)
        self.play(FadeOut(grp, shift=UP * 0.3), FadeIn(hdr), run_time=0.5)

    # -- beat playback ----------------------------------------------------
    def _run_beat(self, beat, prev):
        start, dur = beat["start"], beat["duration"]
        end = start + dur
        self._hold, self._post = [], []
        strays, self._strays = self._strays, []
        btype = beat["type"]

        if beat.get("continue") and prev is not None:
            anims = getattr(self, f"_c_{btype}")(beat, prev)
            rt = min(0.8, max(0.35, dur * 0.2))
            if anims:
                self.play(*anims, run_time=rt)
            for p in self._post:
                p()
            self._run_hold(end)
            return prev

        mob = getattr(self, f"_r_{btype}")(beat)
        fade_t = 0.0 if prev is None else min(0.35, dur * 0.12)
        in_t = min(1.4 if btype in ("flow", "wiring", "board", "stack",
                                    "hacard", "avi", "memory", "motion")
                   else 1.0, max(0.4, dur * 0.3))
        if fade_t > 0:
            self.play(FadeOut(prev), *[FadeOut(m) for m in strays],
                      run_time=fade_t)
        if btype == "bullets":
            self._play_bullets(mob, end - self._now() - 0.2)
        else:
            self.play(self._enter(btype, mob, beat), run_time=in_t)
        for p in self._post:
            p()
        self._run_hold(end)
        return mob

    def _run_hold(self, end: float) -> None:
        """Play hold animations (each returns seconds spent) while time allows."""
        for fn in self._hold:
            left = end - self._now()
            if left < 0.5:
                break
            fn(left - 0.15)
        self._wait_until(end)

    def _enter(self, btype, mob, beat):
        parts = list(mob.submobjects)
        if btype in ("title", "statement", "outro", "endscreen"):
            return LaggedStart(*[Create(p) if isinstance(p, Line)
                                 else FadeIn(p, shift=UP * 0.3) for p in parts],
                               lag_ratio=0.22)
        if btype in ("grid", "stats"):
            head, cards = parts[0], parts[1]
            anims = [FadeIn(head, shift=UP * 0.3)]
            if btype == "stats":
                anims += [FadeIn(VGroup(c[0], c[2]), scale=0.8) for c in cards]
            else:
                anims += [FadeIn(c, scale=0.8, shift=UP * 0.15) for c in cards]
            anims += [FadeIn(p, shift=UP * 0.2) for p in parts[2:]]
            return LaggedStart(*anims, lag_ratio=0.12)
        if btype == "pipeline":
            head, boxes, arrows = parts
            anims = [FadeIn(head, shift=UP * 0.3)]
            for i, b in enumerate(boxes):
                anims.append(FadeIn(b, scale=0.75))
                if i < len(arrows):
                    anims.append(GrowFromEdge(arrows[i], LEFT))
            return LaggedStart(*anims, lag_ratio=0.15)
        if btype == "flow":
            return self._flow_enter(mob)
        if hasattr(mob, "enter_anim"):
            return mob.enter_anim
        return FadeIn(mob, shift=UP * 0.25)

    def _play_bullets(self, mob, budget: float) -> None:
        items = list(mob.submobjects[0:1]) + list(mob.submobjects[1])
        per = max(0.3, budget / max(1, len(items)))
        for i, it in enumerate(items):
            t = min(0.45, per * 0.5)
            anims = [FadeIn(it, shift=RIGHT * 0.25 if i else UP * 0.2)]
            self.play(*anims, run_time=t)
            if per - t > 0.03:
                self.wait(per - t)
        self.add(mob)

    # ======================================================================
    # simple beats
    # ======================================================================
    def _r_title(self, beat):
        parts = []
        if beat.get("logo"):
            parts.append(logo(0.85))
        if beat.get("eyebrow"):
            parts.append(_text(beat["eyebrow"].upper(), 24, T.ACCENT, bold=True))
        parts.append(_text(beat["text"], 80, T.TEXT, bold=True))
        parts.append(Line(LEFT * 1.6, RIGHT * 1.6, stroke_color=T.ACCENT,
                          stroke_width=5))
        if beat.get("sub"):
            parts.append(_text(beat["sub"], 30, T.TEXT_DIM))
        g = VGroup(*parts).arrange(DOWN, buff=0.3).move_to([0, 0, 0])
        return g

    def _r_statement(self, beat):
        parts = []
        if beat.get("icon"):
            ic = icon(beat["icon"], 1.1)
            ring = Circle(radius=0.85, stroke_color=T.ACCENT_DIM,
                          stroke_width=3, fill_color=T.CARD, fill_opacity=1)
            ic.move_to(ring)
            parts.append(VGroup(ring, ic))
        parts.append(_text(beat["text"], 56, T.TEXT, bold=True))
        if beat.get("sub"):
            parts.append(_text(beat["sub"], 32, T.TEXT_DIM))
        return VGroup(*parts).arrange(DOWN, buff=0.42).move_to([0, -0.1, 0])

    def _r_bullets(self, beat):
        head = _heading(beat["heading"])
        rows = VGroup()
        for i, item in enumerate(beat["items"]):
            if beat.get("numbered"):
                mk = VGroup(Circle(radius=0.27, fill_color=T.ACCENT_DIM,
                                   fill_opacity=1, stroke_color=T.ACCENT,
                                   stroke_width=2))
                mk.add(_text(str(i + 1), 22, T.TEXT, bold=True).move_to(mk[0]))
            elif beat.get("check"):
                mk = VGroup(Circle(radius=0.27, fill_color=T.SUCCESS,
                                   fill_opacity=0.18, stroke_color=T.SUCCESS,
                                   stroke_width=2))
                mk.add(icon("check", 0.5, T.SUCCESS).move_to(mk[0]))
            else:
                mk = Square(0.16, fill_color=T.ACCENT, fill_opacity=1,
                            stroke_width=0)
            txt = _text(item, 36, T.TEXT, align="left", max_width=10.5)
            rows.add(VGroup(mk, txt).arrange(RIGHT, buff=0.36))
        rows.arrange(DOWN, buff=0.36, aligned_edge=LEFT)
        g = VGroup(head, rows).arrange(DOWN, buff=0.55)
        g.move_to([0, -0.2, 0])
        return g

    def _r_pipeline(self, beat):
        steps = beat["steps"]
        n = len(steps)
        gap = 0.55
        bw = min(3.2, (13.2 - gap * (n - 1)) / n)
        boxes = VGroup()
        for i, s in enumerate(steps):
            b = _card(bw, 1.45, fill=T.CARD, stroke=T.ACCENT_DIM, sw=2)
            num = _text(f"{i + 1:02d}", 18, T.ACCENT, bold=True)
            t = _text(s, 26, T.TEXT, max_width=bw - 0.3)
            _fit(t, bw - 0.3, 0.85)
            inner = VGroup(num, t).arrange(DOWN, buff=0.14).move_to(b)
            boxes.add(VGroup(b, inner))
        boxes.arrange(RIGHT, buff=gap)
        arrows = VGroup()
        for i in range(n - 1):
            a = boxes[i].get_right() + RIGHT * 0.06
            c = boxes[i + 1].get_left() + LEFT * 0.06
            ln = Line(a, c, stroke_color=T.ACCENT, stroke_width=3)
            ln.add_tip(tip_length=0.14, tip_width=0.14)
            arrows.add(ln)
        head = _heading(beat["heading"])
        g = VGroup(head, boxes, arrows)
        head.move_to([0, 1.5, 0])
        VGroup(boxes, arrows).move_to([0, -0.3, 0])
        # hold: a pulse runs along the steps once
        def hold(left, boxes=boxes):
            per = min(0.35, left / (n + 1))
            if per < 0.15:
                return
            self.play(Succession(*[Indicate(b, color=T.ACCENT_STRONG,
                                            scale_factor=1.06)
                                   for b in boxes]), run_time=per * n)
        self._hold.append(hold)
        return g

    def _r_grid(self, beat):
        items = beat["items"]
        cols = beat.get("cols", 3)
        rows = -(-len(items) // cols)
        gx, gy = 0.38, 0.38
        cw = min(4.0, (13.2 - gx * (cols - 1)) / cols)
        ch = 1.9 if rows <= 2 else 1.6
        col = T.HA if beat.get("accent") == "ha" else T.ACCENT
        cards = VGroup()
        for i, (ic, label) in enumerate(items):
            r, c = divmod(i, cols)
            b = _card(cw, ch, fill=T.CARD, stroke=T.BORDER)
            icn = icon(ic, 0.62, T.TEXT_MUTED if beat.get("dim") else col)
            t = _text(label, 26, T.TEXT_MUTED if beat.get("dim") else T.TEXT,
                      max_width=cw - 0.35)
            _fit(t, cw - 0.35, 0.7)
            inner = VGroup(icn, t).arrange(DOWN, buff=0.24).move_to(b)
            card = VGroup(b, inner)
            if beat.get("dim"):
                b.set_fill(T.PANEL)
            in_row = min(cols, len(items) - r * cols)
            card.move_to([(c - (in_row - 1) / 2) * (cw + gx),
                          -(r - (rows - 1) / 2) * (ch + gy), 0])
            cards.add(card)
        head = _heading(beat["heading"], col)
        parts = [head, cards]
        if beat.get("tag"):
            parts.append(_pill(beat["tag"], T.WARNING, 22))
        if beat.get("arrow"):
            arr = VGroup(
                Line(UP * 0.35, DOWN * 0.35, stroke_color=T.ACCENT,
                     stroke_width=5).add_tip(tip_length=0.22, tip_width=0.22),
                _text(beat["arrow"], 30, T.ACCENT, bold=True))
            arr.arrange(RIGHT, buff=0.3)
            parts.append(arr)
            down = arr[0]

            def hold(left, down=down):
                k = int(min(3, left // 1.0))
                for _ in range(k):
                    self.play(down.animate.shift(DOWN * 0.15), run_time=0.3,
                              rate_func=there_and_back)
            self._hold.append(hold)
        g = VGroup(*parts).arrange(DOWN, buff=0.5)
        g.move_to([0, -0.25, 0])
        if g.height > 5.9:
            g.scale(5.9 / g.height)
        return g

    def _r_code(self, beat):
        lines = VGroup(*[_text(ln, 28, T.TEXT_DIM if not ln.startswith(">")
                               else T.SUCCESS, mono=True, align="left",
                               max_width=20) for ln in beat["lines"]])
        lines.arrange(DOWN, buff=0.22, aligned_edge=LEFT)
        pad = 0.5
        card = _card(max(lines.width + pad * 2, 9), lines.height + pad * 2 + 0.4,
                     fill="#0a0d11", stroke=T.BORDER)
        dots = VGroup(*[Dot(radius=0.07, color=c) for c in
                        (T.DANGER, T.WARNING, T.SUCCESS)]).arrange(RIGHT, buff=0.14)
        dots.move_to(card.get_corner(UP + LEFT) + RIGHT * 0.45 + DOWN * 0.27)
        lines.next_to(card.get_corner(UP + LEFT), DOWN + RIGHT,
                      buff=0).shift(RIGHT * pad + DOWN * (pad + 0.3))
        head = _heading(beat["heading"])
        body = VGroup(card, dots)
        g = VGroup(head, VGroup(body, lines)).arrange(DOWN, buff=0.5)
        g.move_to([0, -0.25, 0])
        if g.width > 13.4:
            g.scale(13.4 / g.width)
        g.enter_anim = AnimationGroup(FadeIn(head, shift=UP * 0.3),
                                      FadeIn(body, scale=0.96), lag_ratio=0.3)

        def post():
            if beat.get("typed"):
                for ln in lines:
                    rt = min(0.8, 0.022 * len(ln))
                    self.play(AddTextLetterByLetter(ln, time_per_char=0.02),
                              run_time=rt)
            else:
                self.play(FadeIn(lines), run_time=0.4)
        self._post.append(post)
        return VGroup(head, body, lines)

    # ======================================================================
    # charts
    # ======================================================================
    def _r_chart(self, beat):
        items = beat["items"]
        n = len(items)
        mx = beat["max"]
        dec = beat.get("decimals", 0)
        unit = beat.get("unit", "")
        bar_h = min(0.5, 4.4 / n * 0.62)
        step = min(0.85, 4.4 / n)
        max_w = 8.0
        label_x = -2.4
        rows = VGroup()
        grows, counts = [], []
        for i, (lab, val) in enumerate(items):
            y = (n - 1) / 2 * step - i * step
            lt = _text(lab, 26 if n > 6 else 30, T.TEXT_DIM)
            lt.move_to([label_x - 0.3 - lt.width / 2, y, 0])
            track = RoundedRectangle(corner_radius=bar_h / 2 * 0.5,
                                     width=max_w, height=bar_h,
                                     fill_color=T.PANEL, fill_opacity=1,
                                     stroke_width=0)
            track.move_to([label_x + max_w / 2, y, 0])
            w = max(0.05, max_w * val / mx)
            frac = i / max(1, n - 1)
            color = T.ACCENT if val < mx else T.ACCENT_STRONG
            if beat.get("highlight_first") or (i == 0 and n > 6):
                color = T.ACCENT_STRONG
            bar = RoundedRectangle(corner_radius=bar_h / 2 * 0.5, width=w,
                                   height=bar_h, fill_color=color,
                                   fill_opacity=1 - 0.25 * frac,
                                   stroke_width=0)
            bar.move_to([label_x + w / 2, y, 0])
            num, anim = _counter(val, dec, 24 if n > 6 else 30, T.TEXT,
                                 unit)
            num.move_to([label_x + w + 0.25 + num.width / 2, y, 0])
            rows.add(VGroup(lt, track, bar, num))
            grows.append(GrowFromEdge(bar, LEFT))
            counts.append(anim)
        head = _heading(beat["heading"])
        cap = _text(beat.get("caption", ""), 26, T.TEXT_MUTED)
        rows.move_to([0, -0.15, 0])
        head.move_to([0, rows.get_top()[1] + 0.6, 0])
        cap.move_to([0, rows.get_bottom()[1] - 0.5, 0])
        g = VGroup(head, rows, cap)
        if g.height > 6.1:
            g.scale(6.1 / g.height).move_to([0, -0.25, 0])
        g.enter_anim = AnimationGroup(
            FadeIn(head, shift=UP * 0.3),
            LaggedStart(*[FadeIn(VGroup(r[0], r[1])) for r in rows],
                        lag_ratio=0.06),
            lag_ratio=0.4)

        def post():
            rt = 1.2
            self.play(LaggedStart(*grows, lag_ratio=0.08),
                      *[c(rt) for c in counts], run_time=rt)
            self.play(FadeIn(cap, shift=UP * 0.15), run_time=0.4)
        self._post.append(post)
        return g

    PARTS = [("boot", "boot", 36, T.TEXT_MUTED), ("nvs", "nvs", 92, T.WARNING),
             ("ota_0", "ota_0  ·  app", 1536, T.ACCENT),
             ("ota_1", "ota_1  ·  app", 1536, "#3b7fd6"),
             ("otadata", "otadata", 8, T.SUCCESS), ("phy", "phy", 4, T.HA),
             ("storage", "storage", 884, "#8b5cf6")]

    def _r_stack(self, beat):
        total = 4096.0
        W, H = 12.6, 1.1
        x = -W / 2
        blocks = {}
        labels = VGroup()
        bar = VGroup()
        callouts = VGroup()
        small = [("boot", -1.25), ("nvs", -1.25), ("otadata", -1.25),
                 ("phy", -1.25)]
        for key, lab, kb, col in self.PARTS:
            w = W * kb / total
            r = Rectangle(width=max(w, 0.02), height=H, fill_color=col,
                          fill_opacity=0.85, stroke_color=T.BG,
                          stroke_width=1.5)
            r.move_to([x + w / 2, 0, 0], aligned_edge=ORIGIN)
            r.move_to([x + w / 2, 0.2, 0])
            bar.add(r)
            blocks[key] = r
            size = f"{kb / 1024:.1f} MB" if kb >= 1024 else f"{kb} KB"
            if w > 1.5:
                t = VGroup(_text(lab, 26, T.TEXT, bold=True),
                           _text(size, 22, T.TEXT)).arrange(DOWN, buff=0.08)
                t.move_to(r)
                labels.add(t)
            x += w
        # callouts for the slivers
        spots = {"boot": -6.3, "nvs": -4.9, "otadata": 2.0, "phy": 3.3}
        for key, _lab, kb, col in self.PARTS:
            if key not in spots:
                continue
            r = blocks[key]
            tx = spots[key]
            t = VGroup(_text(key, 22, col, bold=True),
                       _text(f"{kb} KB", 20, T.TEXT_MUTED)).arrange(DOWN,
                                                                     buff=0.05)
            t.move_to([tx, -1.25, 0])
            ln = _poly((r.get_center()[0], r.get_bottom()[1] - 0.05),
                       (r.get_center()[0], -0.75), (tx, -0.75),
                       (tx, t.get_top()[1] + 0.05), color=col, width=2)
            callouts.add(VGroup(ln, t))
        head = _heading("4 MB flash partition map")
        head.move_to([0, 2.15, 0])
        scale = VGroup(_text("0x000000", 18, T.TEXT_MUTED, mono=True),
                       _text("0x400000", 18, T.TEXT_MUTED, mono=True))
        scale[0].next_to(bar, UP, buff=0.12).align_to(bar, LEFT)
        scale[1].next_to(bar, UP, buff=0.12).align_to(bar, RIGHT)
        info = VGroup()
        info.move_to([0, -2.45, 0])
        g = VGroup(head, scale, bar, labels, callouts, info)
        g.blocks = blocks
        g.info = info
        g.enter_anim = AnimationGroup(
            FadeIn(head, shift=UP * 0.3), FadeIn(scale),
            LaggedStart(*[GrowFromEdge(b, LEFT) for b in bar], lag_ratio=0.2),
            LaggedStart(*[FadeIn(l) for l in labels], lag_ratio=0.2),
            LaggedStart(*[Create(c[0]) for c in callouts], lag_ratio=0.15),
            LaggedStart(*[FadeIn(c[1]) for c in callouts], lag_ratio=0.15),
            lag_ratio=0.25)
        self._post.append(lambda: self.play(*self._stack_focus(g, beat),
                                            run_time=0.6))
        return g

    INFO = {"ota_0": "two 1.5 MB app slots  →  safe over-the-air updates",
            "nvs": "NVS: settings & credentials   ·   storage: reserved for models"}

    def _stack_focus(self, g, beat) -> list:
        focus = set(beat["focus"])
        anims = []
        for k, r in g.blocks.items():
            on = k in focus
            anims.append(r.animate.set_fill(opacity=0.95 if on else 0.25)
                         .set_stroke(T.TEXT if on else T.BG, 3 if on else 1.5))
        text = self.INFO.get(beat["focus"][0], "")
        new = _pill(text, T.ACCENT_STRONG, 24).move_to([0, -2.45, 0])
        if len(g.info.submobjects):
            anims.append(Transform(g.info.submobjects[0], new))
        else:
            g.info.add(new)
            anims.append(FadeIn(new, shift=UP * 0.2))
        return anims

    def _c_stack(self, beat, prev):
        return self._stack_focus(prev, beat)

    def _r_stat(self, beat):
        val = beat["value"]
        num, anim = _counter(val, beat.get("decimals", 0), 58, T.TEXT,
                             beat.get("suffix", ""))
        lab = _text(beat["label"], 32, T.TEXT_DIM)
        ring_bg = Arc(radius=1.65, start_angle=PI / 2, angle=-2 * PI,
                      stroke_color=T.PANEL, stroke_width=26)
        ring = Arc(radius=1.65, start_angle=PI / 2, angle=-2 * PI * val / 100,
                   stroke_color=T.ACCENT, stroke_width=26)
        num.move_to(ORIGIN)
        dial = VGroup(ring_bg, ring, num).move_to([0, 0.45, 0])
        lab.next_to(dial, DOWN, buff=0.45)
        tick = _pill("1.5 MB app slot  ·  room to grow", T.SUCCESS, 22)
        tick.next_to(lab, DOWN, buff=0.3)
        g = VGroup(dial, lab, tick)
        g.enter_anim = FadeIn(VGroup(ring_bg, lab), shift=UP * 0.2)

        def post():
            self.add(num)
            self.play(Create(ring), anim(1.3), run_time=1.3,
                      rate_func=rate_functions.ease_out_cubic)
            self.play(FadeIn(tick, shift=UP * 0.2), run_time=0.4)
        self._post.append(post)
        return g

    def _r_stats(self, beat):
        cards = VGroup()
        counters = []
        for val, lab in beat["items"]:
            b = _card(2.9, 2.3, fill=T.CARD, stroke=T.BORDER)
            dec = 1 if isinstance(val, float) and val % 1 else 0
            num, anim = _counter(val, dec, 70,
                                 T.SUCCESS if val == 0 else T.ACCENT_STRONG)
            t = _text(lab, 26, T.TEXT_DIM)
            VGroup(num, t).arrange(DOWN, buff=0.25).move_to(b)
            cards.add(VGroup(b, num, t))
            counters.append(anim)
        cards.arrange(RIGHT, buff=0.4)
        head = _heading(beat["heading"])
        g = VGroup(head, cards).arrange(DOWN, buff=0.6).move_to([0, -0.15, 0])
        self._post.append(lambda: self.play(*[c(1.4) for c in counters],
                                            run_time=1.4))
        return g

    # ======================================================================
    # flow diagrams
    # ======================================================================
    def _node_mob(self, spec) -> VGroup:
        w, h = spec.get("w", 2.3), spec.get("h", 0.8)
        kind = spec.get("kind", "")
        stroke = {"accent": T.ACCENT, "ha": T.HA,
                  "danger": T.DANGER}.get(kind, T.BORDER)
        fill = {"accent": "#14253b", "ha": "#0f2a36",
                "danger": "#2a1416"}.get(kind, T.CARD)
        box = _card(w, h, fill=fill, stroke=stroke, radius=0.12, sw=2.5)
        lab = _text(spec["label"], 24, T.TEXT, bold=True)
        parts = [lab]
        if spec.get("sub"):
            parts.append(_text(spec["sub"], 17, T.TEXT_MUTED))
        txt = VGroup(*parts).arrange(DOWN, buff=0.06)
        content = txt
        if spec.get("icon"):
            ic = icon(spec["icon"], 0.42,
                      {"ha": T.HA, "danger": T.DANGER}.get(kind, T.ACCENT))
            content = VGroup(ic, txt).arrange(RIGHT, buff=0.18)
        _fit(content, w - 0.25, h - 0.12)
        content.move_to(box)
        g = VGroup(box, content)
        g.move_to([spec["x"], spec["y"], 0])
        g.kind = kind
        return g

    def _edge_pts(self, a, b, route) -> list[np.ndarray]:
        ax, ay = a.get_center()[:2]
        bx, by = b.get_center()[:2]
        P = lambda x, y: np.array([x, y, 0.0])  # noqa: E731
        if route == "straight":
            if abs(ay - by) < 1e-3 or abs(ax - bx) > abs(ay - by):
                s = 1 if bx > ax else -1
                return [P(ax + s * a.width / 2, ay), P(bx - s * b.width / 2, ay)]
            s = 1 if by > ay else -1
            return [P(ax, ay + s * a.height / 2), P(ax, by - s * b.height / 2)]
        if route == "hv":
            s = 1 if bx > ax else -1
            t = 1 if by > ay else -1
            return [P(ax + s * a.width / 2, ay), P(bx, ay),
                    P(bx, by - t * b.height / 2)]
        if route == "vh":
            s = 1 if bx > ax else -1
            t = 1 if by > ay else -1
            return [P(ax, ay + t * a.height / 2), P(ax, by),
                    P(bx - s * b.width / 2, by)]
        if route == "hvh":
            s = 1 if bx > ax else -1
            x0, x1 = ax + s * a.width / 2, bx - s * b.width / 2
            mx = (x0 + x1) / 2
            return [P(x0, ay), P(mx, ay), P(mx, by), P(x1, by)]
        raise ValueError(route)

    def _edge_mob(self, pts) -> VGroup:
        path = VMobject(stroke_color=T.BORDER, stroke_width=3)
        path.set_points_as_corners(pts)
        d = pts[-1] - pts[-2]
        ang = math.atan2(d[1], d[0])
        tip = Polygon([0.0, 0, 0], [-0.2, 0.09, 0], [-0.2, -0.09, 0],
                      fill_color=T.BORDER, fill_opacity=1, stroke_width=0)
        tip.rotate(ang, about_point=ORIGIN).shift(pts[-1])
        g = VGroup(path, tip)
        g.path = path
        return g

    def _build_flow(self, name: str) -> dict:
        spec = DIAGRAMS[name]
        nodes = {k: self._node_mob(v) for k, v in spec["nodes"].items()}
        edges = {}
        for a, b, route in spec["edges"]:
            pts = self._edge_pts(nodes[a], nodes[b], route)
            edges[f"{a}>{b}"] = self._edge_mob(pts)
        return {"nodes": nodes, "edges": edges}

    def _r_flow(self, beat):
        name = beat["diagram"]
        d = self._build_flow(name)
        head = _heading(beat.get("heading", ""))
        head.move_to([0, 2.55, 0])
        diagram = VGroup(VGroup(*d["edges"].values()), VGroup(*d["nodes"].values()))
        diagram.shift(DOWN * 0.25)
        extras = VGroup()
        g = VGroup(head, diagram, extras)
        g.flow = d
        g.extras = extras
        g.name = name
        self._flow_apply(g, beat, animate=False)
        self._post.append(lambda: self._flow_decorate(g, beat))
        self._flow_holds(g, beat)
        return g

    def _flow_enter(self, g):
        d = g.flow
        anims = [FadeIn(g[0], shift=UP * 0.3)]
        anims += [FadeIn(n, scale=0.8) for n in d["nodes"].values()]
        anims += [Create(e) for e in d["edges"].values()]
        return LaggedStart(*anims, lag_ratio=0.07)

    def _lit_sets(self, g, beat):
        d = g.flow
        nodes = set(beat.get("nodes") or d["nodes"].keys())
        edges = set(beat.get("edges") or [
            k for k in d["edges"]
            if k.split(">")[0] in nodes and k.split(">")[1] in nodes])
        bad = (nodes - d["nodes"].keys()) | (edges - d["edges"].keys())
        if bad:
            raise KeyError(f"{g.name}: unknown ids {sorted(bad)}")
        return nodes, edges

    def _flow_apply(self, g, beat, animate: bool) -> list:
        d = g.flow
        nodes, edges = self._lit_sets(g, beat)
        crossed = set(beat.get("crossed", [])) | set(beat.get("offline", []))
        anims = []
        for k, n in d["nodes"].items():
            on = k in nodes
            op = 1.0 if on else (0.55 if k in crossed else 0.28)
            if animate:
                anims.append(n.animate.set_opacity(op))
            else:
                n.set_opacity(op)
        for k, e in d["edges"].items():
            on = k in edges
            tgt = k.split(">")[1]
            col = T.ACCENT if on else (T.DANGER if tgt in crossed else T.BORDER)
            op = 1.0 if on else (0.6 if tgt in crossed else 0.3)
            if animate:
                anims.append(e[0].animate.set_stroke(col, 4 if on else 3, op))
                anims.append(e[1].animate.set_fill(col, op))
            else:
                e[0].set_stroke(col, 4 if on else 3, op)
                e[1].set_fill(col, op)
        return anims

    def _flow_decorate(self, g, beat) -> None:
        d = g.flow
        new = []
        for k in beat.get("crossed", []):
            n = d["nodes"][k]
            x = VGroup(Circle(radius=0.24, fill_color=T.DANGER, fill_opacity=1,
                              stroke_color=T.BG, stroke_width=3),
                       Line([-0.1, -0.1, 0], [0.1, 0.1, 0]),
                       Line([-0.1, 0.1, 0], [0.1, -0.1, 0]))
            x[1:].set_stroke("#ffffff", 5)
            x.move_to(n.get_corner(UP + RIGHT) + [-0.05, -0.02, 0])
            n[0].set_stroke(T.DANGER)
            new.append(x)
        for k in beat.get("crossed_edges", []):
            e = d["edges"][k]
            p = e.path.point_from_proportion(0.5)
            x = VGroup(Circle(radius=0.26, fill_color=T.BG, fill_opacity=1,
                              stroke_color=T.DANGER, stroke_width=4),
                       icon("cross", 0.4)[1:]).move_to(p)
            new.append(x)
        for k in beat.get("offline", []):
            n = d["nodes"][k]
            tag = _pill("offline", T.DANGER, 18).next_to(n, DOWN, buff=0.14)
            new.append(tag)
        for k, text, *side in beat.get("tags", []):
            n = d["nodes"][k]
            tag = _pill(text, T.ACCENT_STRONG, 20)
            where = {"up": UP, "left": LEFT, "down": DOWN}[
                side[0] if side else "down"]
            tag.next_to(n, where, buff=0.22)
            if tag.get_bottom()[1] < CONTENT_BOTTOM:
                tag.next_to(n, UP, buff=0.22)
            new.append(tag)
        if not new:
            return
        for m in new:
            g.extras.add(m)
        self.play(LaggedStart(*[Create(m) if isinstance(m[0], Line)
                                else FadeIn(m, scale=0.8) for m in new],
                              lag_ratio=0.25), run_time=min(1.0, 0.35 * len(new)))
        if beat.get("offline"):
            ns = [d["nodes"][k][0] for k in beat["offline"]]
            self.play(*[n.animate.set_stroke(T.DANGER, 3) for n in ns],
                      run_time=0.3)

    def _flow_holds(self, g, beat) -> None:
        pulse = beat.get("pulse") or []
        if not pulse:
            return
        d = g.flow
        targets = {k.split(">")[1] for k in pulse}
        depth: dict[str, int] = {}

        def dep(k: str) -> int:
            if k not in depth:
                src = k.split(">")[0]
                ups = [p for p in pulse if p.split(">")[1] == src]
                depth[k] = 0 if (src not in targets or not ups) else \
                    1 + max(dep(p) for p in ups if p != k)
            return depth[k]
        for k in pulse:
            dep(k)
        levels = max(depth.values()) + 1

        def hold(left):
            per = 0.75
            cycle = per * levels + 0.35
            spent = 0.0
            for _ in range(4):
                if left - spent < cycle:
                    break
                steps = []
                for lv in range(levels):
                    anims = []
                    for k in pulse:
                        if depth[k] != lv:
                            continue
                        e = d["edges"][k]
                        dot = Dot(radius=0.09, color=T.ACCENT_STRONG)
                        dot.move_to(e.path.get_start())
                        anims.append(Succession(
                            FadeIn(dot, run_time=0.01),
                            MoveAlongPath(dot, e.path.copy(),
                                          rate_func=rate_functions.linear),
                            FadeOut(dot, run_time=0.01)))
                    steps.append(AnimationGroup(*anims))
                self.play(Succession(*steps), run_time=per * levels)
                self.wait(0.35)
                spent += cycle
        self._hold.append(hold)

    def _c_flow(self, beat, prev):
        if beat.get("heading"):
            new = _heading(beat["heading"]).move_to(prev[0])
            anims = [Transform(prev[0], new)]
        else:
            anims = []
        if len(prev.extras.submobjects):
            anims.append(FadeOut(prev.extras))
            prev.remove(prev.extras)
            prev.extras = VGroup()
            prev.add(prev.extras)
        anims += self._flow_apply(prev, beat, animate=True)
        self._post = [lambda: self._flow_decorate(prev, beat)]
        self._flow_holds(prev, beat)
        return anims

    # ======================================================================
    # hardware: board, wiring
    # ======================================================================
    def _esp32cam(self, s: float = 1.0, pins: bool = True) -> VGroup:
        """Top view of an AI-Thinker ESP32-CAM (27 x 40 mm), pins labelled."""
        W, H = 2.7 * s, 4.0 * s
        pcb = RoundedRectangle(corner_radius=0.12 * s, width=W, height=H,
                               fill_color=T.PCB, fill_opacity=1,
                               stroke_color=T.PCB_EDGE, stroke_width=2)
        shield = RoundedRectangle(corner_radius=0.05 * s,
                                  width=(1.05 if pins else 1.7) * s,
                                  height=1.55 * s, fill_color="#9ca3af",
                                  fill_opacity=1, stroke_color="#d1d5db",
                                  stroke_width=1.5)
        shield.move_to(pcb.get_top() + DOWN * 1.0 * s)
        stext = _text("ESP32-S", int(20 * s), "#1f2937", bold=True)
        _fit(stext, shield.width - 0.1)
        stext.move_to(shield)
        cam = RoundedRectangle(corner_radius=0.05 * s, width=1.0 * s,
                               height=1.0 * s, fill_color="#111827",
                               fill_opacity=1, stroke_color="#374151",
                               stroke_width=1.5)
        cam.move_to(pcb.get_center() + DOWN * 0.45 * s)
        lens = VGroup(Circle(radius=0.32 * s, fill_color="#1f2937",
                             fill_opacity=1, stroke_color="#4b5563",
                             stroke_width=2),
                      Circle(radius=0.17 * s, fill_color="#0b1220",
                             fill_opacity=1, stroke_color=T.ACCENT,
                             stroke_width=1.5)).move_to(cam)
        sd = RoundedRectangle(corner_radius=0.04 * s,
                              width=(0.95 if pins else 1.5) * s,
                              height=0.75 * s, fill_color="#cbd5e1",
                              fill_opacity=1, stroke_width=0)
        sd.move_to(pcb.get_bottom() + UP * 0.6 * s)
        sdt = _text("microSD", int(16 * s), "#334155", bold=True)
        _fit(sdt, sd.width - 0.08)
        sdt.move_to(sd)
        led = Circle(radius=0.13 * s, fill_color="#fef9c3", fill_opacity=1,
                     stroke_color="#fde047", stroke_width=2)
        led.move_to(pcb.get_bottom() + UP * 1.35 * s + LEFT * 0.75 * s)
        board = VGroup(pcb, shield, stext, cam, lens, sd, sdt)
        if not pins:
            board.add(led)
        board.parts = {"pcb": pcb, "esp": shield, "cam": cam, "lens": lens,
                       "sd": sd, "led": led}
        board.pins = {}
        if pins:
            left = ["5V", "GND", "IO12", "IO13", "IO15", "IO14", "IO2", "IO4"]
            right = ["3V3", "IO16", "IO0", "GND", "VCC", "U0R", "U0T", "GND"]
            ys = np.linspace(-0.25 * s, -3.75 * s, 8) + H / 2
            for side, names in ((-1, left), (1, right)):
                for name, y in zip(names, ys):
                    p = Square(0.13 * s, fill_color=T.PIN, fill_opacity=1,
                               stroke_width=0)
                    p.move_to([side * (W / 2 - 0.15 * s), y, 0])
                    t = _text(name, int(17 * s), T.TEXT_DIM, bold=True)
                    _fit(t, 0.42 * s)
                    t.next_to(p, RIGHT if side < 0 else LEFT, buff=0.08 * s)
                    board.add(p, t)
                    board.pins[(side, name, round(y, 3))] = p
        return board

    def _pin(self, board, side: int, name: str):
        for (sd, nm, _y), p in board.pins.items():
            if sd == side and nm == name:
                return p
        raise KeyError(name)

    def _laptop(self, s=1.0) -> VGroup:
        scr = RoundedRectangle(corner_radius=0.08, width=2.6 * s,
                               height=1.65 * s, fill_color="#0a0d11",
                               fill_opacity=1, stroke_color=T.TEXT_MUTED,
                               stroke_width=3)
        base = _poly((-1.6 * s, 0), (1.6 * s, 0), (1.4 * s, 0.18 * s),
                     (-1.4 * s, 0.18 * s), closed=True, color=T.TEXT_MUTED,
                     width=3, fill=T.RAISED)
        base.next_to(scr, DOWN, buff=0.02)
        return VGroup(scr, base)

    def _wire(self, pts, color, width=6) -> VMobject:
        m = VMobject(stroke_color=color, stroke_width=width)
        m.set_points_as_corners([np.array([p[0], p[1], 0.0]) for p in pts])
        return m

    def _r_board(self, beat):
        b = self._esp32cam(1.05, pins=False).move_to([-4.3, -0.35, 0])
        P = b.parts
        callouts = [
            (P["esp"], "ESP32, dual core", "the original ESP32, not an S3",
             (-0.6, 1.55)),
            (P["lens"], "OV2640 camera", "JPEG sensor, up to 2 MP",
             (-0.6, 0.4)),
            (P["esp"], "4 MB PSRAM", "external RAM for frames",
             (-0.6, -0.75)),
            (P["sd"], "microSD slot", "SDMMC in 1-bit mode", (-0.6, -1.9)),
        ]
        items = VGroup()
        for target, title, sub, (x, y) in callouts:
            t = VGroup(_text(title, 34, T.TEXT, bold=True),
                       _text(sub, 24, T.TEXT_MUTED)).arrange(DOWN, buff=0.08,
                                                            aligned_edge=LEFT)
            t.move_to([x + t.width / 2, y, 0])
            dot = Dot(target.get_center() + (RIGHT * 0.3 if title.startswith(
                "4 MB") else ORIGIN), radius=0.07, color=T.ACCENT)
            ln = _poly(dot.get_center(), (x - 0.6, dot.get_center()[1]),
                       (x - 0.6, y), (x - 0.12, y), color=T.ACCENT, width=2.5)
            items.add(VGroup(dot, ln, t))
        head = _heading("AI-Thinker ESP32-CAM").move_to([0, 2.55, 0])
        g = VGroup(head, b, items)
        g.enter_anim = AnimationGroup(
            FadeIn(head, shift=UP * 0.3), FadeIn(b, shift=UP * 0.3),
            LaggedStart(*[AnimationGroup(FadeIn(i[0], scale=2), Create(i[1]),
                                         FadeIn(i[2], shift=LEFT * 0.2))
                          for i in items], lag_ratio=0.35), lag_ratio=0.3)
        return g

    def _r_wiring(self, beat):
        v = beat["variant"]
        return getattr(self, f"_wiring_{v}")(beat)

    def _wiring_mb(self, beat):
        head = _heading("Programming: ESP32-CAM-MB adapter").move_to([0, 2.55, 0])
        board = self._esp32cam(0.72, pins=False)
        mb = RoundedRectangle(corner_radius=0.1, width=2.7 * 0.72 + 0.25,
                              height=2.2, fill_color="#7f1d1d", fill_opacity=1,
                              stroke_color="#b91c1c", stroke_width=2)
        chip = Rectangle(width=0.7, height=0.45, fill_color="#111827",
                         fill_opacity=1, stroke_width=0)
        chipt = _text("CH340", 14, T.TEXT_MUTED, bold=True)
        usb = RoundedRectangle(corner_radius=0.05, width=0.6, height=0.3,
                               fill_color="#9ca3af", fill_opacity=1,
                               stroke_width=0)
        btns = VGroup(*[VGroup(Circle(radius=0.12, fill_color="#1f2937",
                                      fill_opacity=1, stroke_color="#6b7280",
                                      stroke_width=2),
                               _text(t, 13, T.TEXT_DIM, bold=True))
                        for t in ("IO0", "RST")])
        for bt in btns:
            bt[1].next_to(bt[0], DOWN, buff=0.06)
        btns.arrange(RIGHT, buff=0.4)
        mbt = _text("ESP32-CAM-MB", 18, T.TEXT, bold=True)
        mb.move_to([-3.2, -1.65, 0])
        mbt.next_to(mb, DOWN, buff=0.12)
        chip.move_to(mb.get_center() + DOWN * 0.05)
        chipt.move_to(chip)
        btns.move_to(mb.get_center() + DOWN * 0.68)
        usb.move_to([mb.get_right()[0] - 0.05, mb.get_center()[1] - 0.05, 0])
        mbg = VGroup(mb, mbt, chip, chipt, btns, usb)
        board.move_to([-3.2, 1.0, 0])
        lap = self._laptop(1.5).move_to([3.7, -0.6, 0])
        term = VGroup(*[_text(t, 17, c, mono=True, align="left") for t, c in (
            ("$ pio run -t upload", T.SUCCESS),
            ("Writing at 0x00020000...", T.TEXT_DIM),
            ("$ pio device monitor", T.SUCCESS))]).arrange(DOWN, buff=0.1,
                                                           aligned_edge=LEFT)
        _fit(term, lap[0].width - 0.3)
        term.move_to(lap[0])
        cable = self._wire([usb.get_right(), (usb.get_right()[0] + 1.6,
                                              usb.get_right()[1]),
                            (2.0, lap[1].get_center()[1]),
                            lap[1].get_left()], T.TEXT_MUTED, 7)
        lab = _pill("USB serial: flash + monitor", T.ACCENT_STRONG, 22)
        lab.move_to([0.9, 1.5, 0])
        note = _text("the board plugs onto the adapter", 24,
                     T.TEXT_MUTED).move_to([3.4, -2.6, 0])
        arrow = Line(UP * 0.0, DOWN * 0.55, stroke_color=T.ACCENT,
                     stroke_width=5).add_tip(tip_length=0.2, tip_width=0.2)
        arrow.move_to([-1.2, -0.1, 0])
        g = VGroup(head, board, mbg, lap, term, cable, lab, note, arrow)
        g.enter_anim = AnimationGroup(
            FadeIn(head, shift=UP * 0.3), FadeIn(mbg, shift=UP * 0.3),
            FadeIn(board, shift=DOWN * 0.4), GrowFromEdge(arrow, UP),
            FadeIn(note), lag_ratio=0.3)

        def post():
            self.play(board.animate.shift(DOWN * 0.35), run_time=0.4)
            self.play(Create(cable), FadeIn(lap), run_time=0.8)
            self.play(FadeIn(lab, scale=0.8),
                      LaggedStart(*[FadeIn(t) for t in term], lag_ratio=0.4),
                      run_time=0.9)
        self._post.append(post)
        return g

    def _wiring_pins(self, beat):
        head = _heading("Inside the board: who owns which pin").move_to(
            [0, 2.55, 0])
        esp = _card(2.6, 4.6, fill="#1f2937", stroke=T.TEXT_MUTED, sw=2)
        esp.move_to([0, -0.4, 0])
        espt = VGroup(_text("ESP32", 30, T.TEXT, bold=True),
                      _text("GPIO matrix", 18, T.TEXT_MUTED)).arrange(
            DOWN, buff=0.08).move_to(esp.get_top() + DOWN * 0.6)
        cam = _card(2.4, 2.6, fill=T.CARD, stroke=T.ACCENT, sw=2)
        cam.move_to([-4.85, 0.25, 0])
        camt = VGroup(icon("camera", 0.6), _text("OV2640", 24, T.TEXT,
                                                 bold=True)).arrange(DOWN,
                                                                     buff=0.15)
        camt.move_to(cam)
        sd = _card(2.4, 1.9, fill=T.CARD, stroke="#8b5cf6", sw=2)
        sd.move_to([4.85, 0.75, 0])
        sdt = VGroup(icon("sd", 0.55, "#a78bfa"), _text("microSD", 24, T.TEXT,
                                                        bold=True)).arrange(
            DOWN, buff=0.12).move_to(sd)
        led = Circle(radius=0.38, fill_color="#fef9c3", fill_opacity=0.25,
                     stroke_color="#fde047", stroke_width=3)
        led.move_to([4.85, -2.2, 0])
        ledt = _text("Flash LED", 22, T.TEXT, bold=True).next_to(led, RIGHT,
                                                                buff=0.2)
        ledt.shift(LEFT * 0.0)
        # camera bus: bundled lines
        bus = VGroup()
        cam_lines = [("SCCB  26 / 27", 1.05), ("D0–D7  5,18,19,21,36,39,34,35",
                                              0.55),
                     ("VSYNC 25 · HREF 23 · PCLK 22", 0.05),
                     ("XCLK 0 · PWDN 32", -0.45)]
        for txt, y in cam_lines:
            w = self._wire([(cam.get_right()[0], y), (esp.get_left()[0], y)],
                           T.ACCENT, 4)
            t = _text(txt, 17, T.TEXT_DIM).next_to(w, UP, buff=0.05)
            _fit(t, 2.25)
            bus.add(VGroup(w, t))
        sd_lines = [("CLK · GPIO14", 1.35, T.WIRE_PURPLE, True),
                    ("CMD · GPIO15", 0.95, T.WIRE_PURPLE, True),
                    ("D0 · GPIO2", 0.55, T.WIRE_PURPLE, True),
                    ("D1 · GPIO4", 0.15, T.WIRE_PURPLE, False)]
        sdw = VGroup()
        d1 = None
        for txt, y, col, used in sd_lines:
            pts = [(esp.get_right()[0], y), (sd.get_left()[0], y)]
            if used:
                w = self._wire(pts, col, 4)
            else:
                w = DashedLine([pts[0][0], y, 0], [pts[1][0], y, 0],
                               dash_length=0.1, stroke_color=T.TEXT_MUTED,
                               stroke_width=3)
            t = _text(txt, 18, T.TEXT_DIM if used else T.TEXT_MUTED)
            t.next_to(w, UP, buff=0.04)
            sdw.add(VGroup(w, t))
            if not used:
                d1 = VGroup(w, t)
        gpio4 = self._wire([(esp.get_right()[0], -2.2), (led.get_left()[0],
                                                         -2.2)], T.WIRE_YELLOW, 5)
        g4t = _text("GPIO4 drives the LED", 17, T.WIRE_YELLOW, bold=True)
        g4t.next_to(gpio4, UP, buff=0.05)
        cross = VGroup(Line([-0.15, -0.15, 0], [0.15, 0.15, 0]),
                       Line([-0.15, 0.15, 0], [0.15, -0.15, 0])).set_stroke(
            T.DANGER, 5).move_to([3.25, 0.15, 0])
        tag = _pill("1-bit mode: CLK, CMD, D0 only", "#a78bfa", 20)
        tag.move_to([4.6, -0.75, 0])
        psram = _text("GPIO16 → PSRAM (reserved)", 17, T.TEXT_MUTED)
        psram.next_to(esp, DOWN, buff=0.15)
        g = VGroup(head, esp, espt, cam, camt, sd, sdt, led, ledt, bus, sdw,
                   gpio4, g4t, cross, tag, psram)
        g.enter_anim = AnimationGroup(
            FadeIn(head, shift=UP * 0.3),
            FadeIn(VGroup(esp, espt), scale=0.9),
            FadeIn(VGroup(cam, camt, sd, sdt, led, ledt), scale=0.9),
            LaggedStart(*[AnimationGroup(Create(b[0]), FadeIn(b[1]))
                          for b in bus], lag_ratio=0.2),
            lag_ratio=0.3)

        def post():
            self.play(LaggedStart(*[AnimationGroup(Create(b[0]), FadeIn(b[1]))
                                    for b in sdw], lag_ratio=0.25),
                      run_time=1.0)
            self.play(Create(gpio4), FadeIn(g4t), run_time=0.6)
            self.play(Create(cross), FadeIn(tag, shift=UP * 0.15),
                      FadeIn(psram), run_time=0.6)

        def hold(left):
            if left < 1.2:
                return
            self.play(led.animate.set_fill("#fef08a", 0.95), run_time=0.3)
            self.play(led.animate.set_fill("#fef9c3", 0.25), run_time=0.4)
        self._post.append(post)
        self._hold.append(hold)
        return g

    def _wiring_serial(self, beat):
        head = _heading("Wiring a USB-serial adapter (alternative to the MB)")
        head.move_to([0, 2.55, 0])
        board = self._esp32cam(0.78).move_to([2.6, -0.45, 0])
        ad = _card(2.3, 3.2, fill="#1e3a8a", stroke="#3b82f6", sw=2)
        ad.move_to([-4.4, -0.45, 0])
        adt = VGroup(_text("USB-serial", 22, T.TEXT, bold=True),
                     _text("3.3 V logic", 17, T.TEXT_DIM)).arrange(
            DOWN, buff=0.06).move_to(ad.get_top() + DOWN * 0.5)
        usb = RoundedRectangle(corner_radius=0.05, width=0.7, height=0.35,
                               fill_color="#9ca3af", fill_opacity=1,
                               stroke_width=0).next_to(ad, LEFT, buff=-0.15)
        names = ["5V", "GND", "TXD", "RXD"]
        apins = {}
        for i, n in enumerate(names):
            y = ad.get_center()[1] + 0.2 - i * 0.5
            p = Square(0.16, fill_color=T.PIN, fill_opacity=1, stroke_width=0)
            p.move_to([ad.get_right()[0] - 0.2, y, 0])
            t = _text(n, 19, T.TEXT, bold=True).next_to(p, LEFT, buff=0.15)
            apins[n] = p
            ad_parts = (p, t)
            adt.add(*ad_parts)
        L = lambda n: self._pin(board, -1, n)  # noqa: E731
        R = lambda n: self._pin(board, 1, n)  # noqa: E731
        wires = []
        # 5V and GND go to the board's left header
        for n, target, col, lane in (("5V", L("5V"), T.WIRE_RED, -1.2),
                                     ("GND", L("GND"), T.WIRE_BLACK, -1.45)):
            a = apins[n].get_center()
            b = target.get_center()
            wires.append(self._wire([a, (lane, a[1]), (lane, b[1]), b], col,
                                    6))
        # TX -> U0R, RX -> U0T go around the board to the right header
        top_y = 2.0
        for n, target, col, lane, rx in (
                ("TXD", R("U0R"), T.WIRE_GREEN, -0.95, 5.05),
                ("RXD", R("U0T"), T.WIRE_YELLOW, -0.7, 5.3)):
            a = apins[n].get_center()
            b = target.get_center()
            wires.append(self._wire([a, (lane, a[1]), (lane, top_y - (
                0.25 if n == "RXD" else 0)), (rx, top_y - (0.25 if n == "RXD"
                                                           else 0)),
                (rx, b[1]), b], col, 6))
        io0 = DashedLine(L("GND").get_center() + LEFT * 0.0,
                         R("IO0").get_center(), dash_length=0.12,
                         stroke_color=T.WARNING, stroke_width=4)
        io0_lab = _pill("IO0 → GND only while flashing", T.WARNING, 18)
        io0_lab.move_to([2.6, -3.05, 0])
        io0 = self._wire([R("IO0").get_center(), (4.75, R("IO0").get_center()[1]),
                          (4.75, -2.75), (0.6, -2.75),
                          (0.6, L("GND").get_center()[1] - 0.0),
                          (L("GND").get_center()[0] - 0.0,
                           L("GND").get_center()[1])], T.WARNING, 3)
        io0.set_stroke(opacity=0.0)
        legend = VGroup(*[VGroup(Line(ORIGIN, RIGHT * 0.45, stroke_color=c,
                                      stroke_width=6),
                                 _text(t, 18, T.TEXT_DIM)).arrange(RIGHT,
                                                                   buff=0.15)
                          for c, t in ((T.WIRE_RED, "5V → 5V"),
                                       (T.WIRE_BLACK, "GND → GND"),
                                       (T.WIRE_GREEN, "TX → U0R"),
                                       (T.WIRE_YELLOW, "RX → U0T"))])
        legend.arrange(DOWN, buff=0.14, aligned_edge=LEFT)
        legend.move_to([-4.4, -2.75, 0])
        lt = _text("115200 baud", 18, T.TEXT_MUTED).next_to(ad, UP, buff=0.15)
        g = VGroup(head, ad, adt, usb, board, legend, lt, io0_lab,
                   *wires)
        g.enter_anim = AnimationGroup(
            FadeIn(head, shift=UP * 0.3),
            FadeIn(VGroup(ad, adt, usb, lt), shift=RIGHT * 0.3),
            FadeIn(board, shift=LEFT * 0.3), lag_ratio=0.3)

        def post():
            self.play(LaggedStart(*[Create(w) for w in wires], lag_ratio=0.35),
                      LaggedStart(*[FadeIn(x, shift=RIGHT * 0.15)
                                    for x in legend], lag_ratio=0.35),
                      run_time=2.0)
            self.play(FadeIn(io0_lab, shift=UP * 0.15), run_time=0.4)

        def hold(left):
            for w in wires[2:]:
                if left < 1.0:
                    return
                dot = Dot(radius=0.08, color=T.TEXT)
                dot.move_to(w.get_start())
                self.play(MoveAlongPath(dot, w.copy()), run_time=0.9,
                          rate_func=rate_functions.linear)
                self.remove(dot)
                left -= 0.9
        self._post.append(post)
        self._hold.append(hold)
        return g

    # ======================================================================
    # concept animations
    # ======================================================================
    def _r_queue(self, beat):
        head = _heading("Two-frame queue, drop oldest").move_to([0, 2.55, 0])
        prod = VGroup(_card(2.4, 1.1, fill="#14253b", stroke=T.ACCENT, sw=2.5),
                      _text("Capture", 26, T.TEXT, bold=True))
        prod[1].move_to(prod[0])
        prod.move_to([-5.0, 0, 0])
        cons = VGroup(_card(2.4, 1.1, fill=T.CARD, stroke=T.WARNING, sw=2.5),
                      VGroup(_text("Slow viewer", 24, T.TEXT, bold=True),
                             _text("falls behind", 17, T.TEXT_MUTED)).arrange(
                          DOWN, buff=0.05))
        cons[1].move_to(cons[0])
        cons.move_to([5.0, 0, 0])
        slots = VGroup(*[RoundedRectangle(corner_radius=0.08, width=1.25,
                                          height=1.25, stroke_color=T.BORDER,
                                          stroke_width=2.5, fill_opacity=0)
                         for _ in range(2)]).arrange(RIGHT, buff=0.25)
        slots.move_to([0, 0, 0])
        frame = _card(3.4, 1.9, fill=T.PANEL, stroke=T.BORDER)
        frame.move_to(slots)
        qt = _text("subscriber queue", 18, T.TEXT_MUTED).next_to(frame, UP,
                                                                 buff=0.1)
        cap = _text("a slow viewer never stalls the camera", 30, T.TEXT_DIM)
        cap.move_to([0, -2.4, 0])
        a1 = Line(prod.get_right(), frame.get_left(), stroke_color=T.BORDER,
                  stroke_width=3)
        a2 = Line(frame.get_right(), cons.get_left(), stroke_color=T.BORDER,
                  stroke_width=3)
        g = VGroup(head, prod, cons, frame, qt, slots, a1, a2, cap)
        self._q = {"slots": slots, "queue": [], "n": 0, "prod": prod,
                   "cons": cons}

        def tile(n):
            t = VGroup(RoundedRectangle(corner_radius=0.08, width=1.1,
                                        height=1.1, fill_color=T.ACCENT,
                                        fill_opacity=0.9, stroke_width=0),
                       _text(f"F{n}", 26, T.BG, bold=True))
            return t

        def push():
            q = self._q
            q["n"] += 1
            t = tile(q["n"])
            t.move_to(q["prod"])
            anims = []
            if len(q["queue"]) == 2:
                old = q["queue"].pop(0)
                anims.append(old.animate.set_fill(T.DANGER).shift(DOWN * 1.3)
                             .set_opacity(0))
                anims.append(q["queue"][0].animate.move_to(q["slots"][0]))
            dest = q["slots"][len(q["queue"])]
            q["queue"].append(t)
            self.add(t)
            anims.append(t.animate.move_to(dest))
            self.play(*anims, run_time=0.55)

        def pop():
            q = self._q
            if not q["queue"]:
                return
            t = q["queue"].pop(0)
            anims = [t.animate.move_to(q["cons"]).set_opacity(0)]
            if q["queue"]:
                anims.append(q["queue"][0].animate.move_to(q["slots"][0]))
            self.play(*anims, run_time=0.55)

        def hold(left):
            seq = [push, push, push, pop, push, push, pop, push, push]
            for fn in seq:
                if left < 0.9:
                    break
                fn()
                self.wait(0.2)
                left -= 0.75
            # the next beat's fade must cover the tiles still on screen
            self._strays.extend(self._q["queue"])
        self._hold.append(hold)
        return g

    def _r_avi(self, beat):
        head = _heading("Standard AVI with Motion JPEG inside").move_to(
            [0, 2.55, 0])
        y = 0.3
        hdr = VGroup(_card(3.2, 1.6, fill="#14253b", stroke=T.ACCENT, sw=2.5),
                     VGroup(_text("RIFF header", 24, T.TEXT, bold=True),
                            _text("hdrl · avih · strh · strf", 17,
                                  T.TEXT_MUTED)).arrange(DOWN, buff=0.08))
        hdr[1].move_to(hdr[0])
        hdr.move_to([-4.6, y, 0])
        movi = _card(6.6, 1.6, fill=T.CARD, stroke=T.BORDER, sw=2)
        movi.move_to([0.65, y, 0])
        mt = _text("movi", 20, T.TEXT_MUTED, bold=True).next_to(movi, UP,
                                                               buff=0.1)
        chunks = VGroup(*[RoundedRectangle(corner_radius=0.05, width=0.42,
                                           height=1.15, fill_color=T.ACCENT,
                                           fill_opacity=0.75, stroke_width=0)
                          for _ in range(12)]).arrange(RIGHT, buff=0.08)
        chunks.move_to(movi)
        ct = _text("00dc: one JPEG frame per chunk", 20, T.TEXT_DIM)
        ct.next_to(movi, DOWN, buff=0.18)
        idx = VGroup(_card(2.0, 1.6, fill="#123323", stroke=T.SUCCESS, sw=2.5),
                     VGroup(_text("idx1", 24, T.TEXT, bold=True),
                            _text("index", 17, T.TEXT_MUTED)).arrange(
                         DOWN, buff=0.08))
        idx[1].move_to(idx[0])
        idx.move_to([5.15, y, 0])
        note = _text("header  →  frames  →  index", 30, T.TEXT_DIM)
        note.move_to([0, -2.2, 0])
        g = VGroup(head, hdr, movi, mt, ct, chunks, idx, note)
        g.enter_anim = AnimationGroup(FadeIn(head, shift=UP * 0.3),
                                      FadeIn(hdr, shift=RIGHT * 0.3),
                                      FadeIn(VGroup(movi, mt, ct)),
                                      lag_ratio=0.3)
        for c in chunks:
            c.set_opacity(0)

        def post():
            for c in chunks:
                c.set_opacity(0.75)
            self.play(LaggedStart(*[FadeIn(c, shift=DOWN * 0.3)
                                    for c in chunks], lag_ratio=0.25),
                      run_time=1.6)
            self.play(FadeIn(idx, shift=LEFT * 0.3), FadeIn(note), run_time=0.5)
        self._post.append(post)
        return g

    def _r_memory(self, beat):
        head = _heading("Where memory lives").move_to([0, 2.55, 0])
        lc = _card(5.6, 4.4, fill=T.CARD, stroke=T.WARNING, sw=2)
        rc = _card(5.6, 4.4, fill=T.CARD, stroke=T.ACCENT, sw=2)
        lc.move_to([-3.2, -0.5, 0])
        rc.move_to([3.2, -0.5, 0])
        lt = VGroup(_text("Internal RAM", 30, T.TEXT, bold=True),
                    _text("small  ·  DMA-capable", 20, T.WARNING)).arrange(
            DOWN, buff=0.06).move_to(lc.get_top() + DOWN * 0.6)
        rt = VGroup(_text("PSRAM", 30, T.TEXT, bold=True),
                    _text("4 MB  ·  external", 20, T.ACCENT)).arrange(
            DOWN, buff=0.06).move_to(rc.get_top() + DOWN * 0.6)

        def chip(t, col):
            c = VGroup(RoundedRectangle(corner_radius=0.08, width=4.6,
                                        height=0.5, fill_color=T.PANEL,
                                        fill_opacity=1, stroke_color=col,
                                        stroke_width=1.5),
                       _text(t, 21, T.TEXT_DIM))
            c[1].move_to(c[0])
            return c
        left = VGroup(*[chip(t, T.WARNING) for t in
                        ("Camera DMA", "SD card DMA", "Task stacks")])
        right = VGroup(*[chip(t, T.ACCENT) for t in
                         ("Camera frames", "RTSP + recorder copies",
                          "MQTT queue")])
        left.arrange(DOWN, buff=0.16).next_to(lt, DOWN, buff=0.35)
        right.arrange(DOWN, buff=0.16).next_to(rt, DOWN, buff=0.35)
        wifi = chip("Wi-Fi + network buffers", T.ACCENT_STRONG)
        wifi.next_to(left, DOWN, buff=0.16)
        wifi_dest = right.get_bottom() + DOWN * (0.16 + 0.25)
        g = VGroup(head, lc, rc, lt, rt, left, right, wifi)
        g.enter_anim = AnimationGroup(
            FadeIn(head, shift=UP * 0.3),
            FadeIn(VGroup(lc, lt), shift=UP * 0.2),
            FadeIn(VGroup(rc, rt), shift=UP * 0.2),
            LaggedStart(*[FadeIn(c, shift=UP * 0.1) for c in
                          [*left, *right, wifi]], lag_ratio=0.12),
            lag_ratio=0.25)

        def post():
            self.wait(0.3)
            self.play(wifi.animate.move_to(wifi_dest), path_arc=-0.6,
                      run_time=1.1)
            self.play(Indicate(wifi, color=T.ACCENT_STRONG, scale_factor=1.05),
                      run_time=0.6)
        self._post.append(post)
        return g

    def _r_motion(self, beat):
        head = _heading("Motion detection, block by block").move_to([0, 2.55, 0])
        rnd = random.Random(3)
        N = 8
        cell = 0.36
        base = [[rnd.uniform(0.15, 0.55) for _ in range(N)] for _ in range(N)]
        changed = {(r, c) for r in range(2, 5) for c in range(4, 7)}

        def grid(vals, x):
            gg = VGroup()
            for r in range(N):
                for c in range(N):
                    v = vals[r][c]
                    sq = Square(cell, fill_color=f"#{int(v * 255):02x}"
                                f"{int(v * 255):02x}{int(v * 255):02x}",
                                fill_opacity=1, stroke_color=T.BG,
                                stroke_width=1)
                    sq.move_to([x + (c - N / 2 + 0.5) * cell,
                                0.1 - (r - N / 2 + 0.5) * cell, 0])
                    gg.add(sq)
            return gg
        cur = [row[:] for row in base]
        for r, c in changed:
            cur[r][c] = min(0.95, base[r][c] + 0.4)
        prev_g = grid(base, -4.6)
        cur_g = grid(cur, 0)
        diff = VGroup()
        for r in range(N):
            for c in range(N):
                on = (r, c) in changed
                sq = Square(cell, fill_color=T.ACCENT if on else T.PANEL,
                            fill_opacity=1, stroke_color=T.BG, stroke_width=1)
                sq.move_to([4.6 + (c - N / 2 + 0.5) * cell,
                            0.1 - (r - N / 2 + 0.5) * cell, 0])
                diff.add(sq)
        labs = VGroup(
            _text("previous sample", 22, T.TEXT_MUTED).next_to(prev_g, UP, 0.2),
            _text("new frame", 22, T.TEXT_MUTED).next_to(cur_g, UP, 0.2),
            _text("changed blocks", 22, T.TEXT_MUTED).next_to(diff, UP, 0.2))
        size = _text("96 × 96 grayscale", 22, T.TEXT_DIM).next_to(cur_g, DOWN,
                                                                 0.2)
        ops = VGroup(_text("vs", 34, T.TEXT_MUTED, bold=True).move_to([-2.3, 0.1, 0]),
                     _text("=", 40, T.TEXT_MUTED, bold=True).move_to([2.3, 0.1, 0]))
        fire = _pill("9 blocks changed  →  motion started", T.WARNING, 26)
        fire.move_to([0, -2.6, 0])
        g = VGroup(head, prev_g, cur_g, labs, size, ops, diff, fire)
        for sq in diff:
            sq.set_opacity(0)
        fire.set_opacity(0)
        g.enter_anim = AnimationGroup(
            FadeIn(head, shift=UP * 0.3),
            FadeIn(prev_g, scale=0.9), FadeIn(cur_g, scale=0.9),
            FadeIn(labs), FadeIn(size), FadeIn(ops), lag_ratio=0.2)

        def post():
            for sq in diff:
                sq.set_opacity(1)
            self.play(LaggedStart(*[FadeIn(sq, scale=0.5) for sq in diff],
                                  lag_ratio=0.01), run_time=1.0)
            fire.set_opacity(1)
            self.play(FadeIn(fire, scale=0.8), run_time=0.4)
            self.play(Indicate(fire, color=T.WARNING), run_time=0.6)
        self._post.append(post)
        return g

    def _r_hacard(self, beat):
        W, H = 6.6, 5.3
        card = _card(W, H, fill="#1c1c1c", stroke="#2c2c2c", radius=0.2, sw=2)
        card.move_to([-2.4, -0.45, 0])
        top = card.get_top()[1]
        title = VGroup(icon("home", 0.45, T.HA),
                       _text("VisionCam Pro", 28, T.TEXT, bold=True),
                       _text("MQTT device", 17, T.TEXT_MUTED))
        title[1:].arrange(DOWN, buff=0.04, aligned_edge=LEFT)
        title.arrange(RIGHT, buff=0.25)
        title.move_to([card.get_left()[0] + 0.35 + title.width / 2,
                       top - 0.5, 0])
        rows_spec = [("motion", "Motion", "Clear"),
                     ("play", "Recording", "Idle"),
                     ("camera", "Camera", "Online"),
                     ("sd", "SD card", "OK"),
                     ("chart", "Storage used", "12 %"),
                     ("wifi", "Wi-Fi signal", "-54 dBm"),
                     ("button", "Take snapshot", "PRESS"),
                     ("bolt", "Flash LED", "toggle")]
        rows = VGroup()
        toggle = None
        press = None
        for i, (ic, name, state) in enumerate(rows_spec):
            y = top - 1.25 - i * 0.5
            ico = icon(ic, 0.32, T.HA).move_to([card.get_left()[0] + 0.55, y, 0])
            nm = _text(name, 21, T.TEXT)
            nm.move_to([ico.get_right()[0] + 0.3 + nm.width / 2, y, 0])
            if state == "toggle":
                tr = RoundedRectangle(corner_radius=0.13, width=0.62,
                                      height=0.26, fill_color="#555",
                                      fill_opacity=1, stroke_width=0)
                kn = Circle(radius=0.15, fill_color="#ddd", fill_opacity=1,
                            stroke_width=0)
                tr.move_to([card.get_right()[0] - 0.65, y, 0])
                kn.move_to(tr.get_left() + RIGHT * 0.12)
                st = VGroup(tr, kn)
                toggle = st
            elif state == "PRESS":
                st = _text("PRESS", 19, T.HA, bold=True)
                st.move_to([card.get_right()[0] - 0.4 - st.width / 2, y, 0])
                press = st
            else:
                st = _text(state, 20, T.TEXT_MUTED)
                st.move_to([card.get_right()[0] - 0.4 - st.width / 2, y, 0])
            rows.add(VGroup(ico, nm, st))
        tile = _card(4.6, 3.1, fill="#05070a", stroke=T.HA, radius=0.15, sw=2)
        tile.move_to([4.0, 0.2, 0])
        # a tiny "live" scene inside the tile
        scene_bg = Rectangle(width=4.4, height=2.9, fill_color="#0f1722",
                             fill_opacity=1, stroke_width=0).move_to(tile)
        door = Rectangle(width=0.8, height=1.5, fill_color="#3b2f25",
                         fill_opacity=1, stroke_width=0).move_to(
            tile.get_center() + LEFT * 1.0 + DOWN * 0.55)
        floor = Rectangle(width=4.4, height=0.5, fill_color="#1b2533",
                          fill_opacity=1, stroke_width=0).move_to(
            tile.get_bottom() + UP * 0.35)
        person = VGroup(Circle(radius=0.14, fill_color="#94a3b8",
                               fill_opacity=1, stroke_width=0),
                        RoundedRectangle(corner_radius=0.1, width=0.36,
                                         height=0.7, fill_color="#64748b",
                                         fill_opacity=1, stroke_width=0)
                        ).arrange(DOWN, buff=0.03)
        person.move_to(tile.get_center() + RIGHT * 1.7 + DOWN * 0.45)
        live = _pill("● LIVE", T.DANGER, 16, fill="#05070a").move_to(
            tile.get_corner(UP + LEFT) + RIGHT * 0.6 + DOWN * 0.3)
        tlabel = _text("Camera feed", 20, T.TEXT_MUTED).next_to(tile, DOWN,
                                                               buff=0.15)
        tilegrp = VGroup(tile, scene_bg, floor, door, person, live, tlabel)
        g = VGroup(card, title, rows, tilegrp)
        g.enter_anim = AnimationGroup(
            FadeIn(VGroup(card, title), shift=UP * 0.2),
            LaggedStart(*[FadeIn(r, shift=RIGHT * 0.25) for r in rows],
                        lag_ratio=0.12),
            FadeIn(tilegrp, scale=0.9), lag_ratio=0.25)

        def hold(left):
            if left < 2.0:
                return
            mrow = rows[0][2]
            new = _text("Detected", 20, T.WARNING, bold=True)
            new.move_to(mrow.get_right() - RIGHT * new.width / 2)
            self.play(person.animate.shift(LEFT * 1.9), Transform(mrow, new),
                      run_time=1.4)
            if left - 1.4 > 1.0:
                tr, kn = toggle
                self.play(tr.animate.set_fill(T.HA), kn.animate.shift(
                    RIGHT * 0.38), run_time=0.4)
                self.play(scene_bg.animate.set_fill("#334155"), run_time=0.3)
            if left - 2.1 > 0.8:
                self.play(Indicate(press, color=T.HA), run_time=0.6)
        self._hold.append(hold)
        return g

    def _r_screenshot(self, beat):
        from PIL import Image
        img = ImageMobject(np.array(
            Image.open(IMAGES / beat["image"]).convert("RGBA")))
        img.set_resampling_algorithm(2)  # bicubic-ish (LANCZOS)
        max_w, max_h = 11.6, 4.3
        img.scale_to_fit_width(max_w)
        if img.height > max_h:
            img.scale_to_fit_height(max_h)
        bar_h = 0.34
        frame = RoundedRectangle(corner_radius=0.12, width=img.width + 0.12,
                                 height=img.height + bar_h + 0.12,
                                 fill_color=T.PANEL, fill_opacity=1,
                                 stroke_color=T.BORDER, stroke_width=2)
        frame.move_to([0, -0.42, 0])
        img.move_to(frame.get_center() + DOWN * bar_h / 2)
        dots = VGroup(*[Dot(radius=0.055, color=c) for c in
                        (T.DANGER, T.WARNING, T.SUCCESS)]).arrange(RIGHT,
                                                                  buff=0.1)
        dots.move_to(frame.get_corner(UP + LEFT) + RIGHT * 0.38
                     + DOWN * (bar_h / 2 + 0.04))
        url = _pill("http://<camera-ip>/", T.TEXT_MUTED, 14, fill=T.CARD)
        url.move_to([0, dots.get_center()[1], 0])
        head = _heading(beat["heading"]).move_to([0, 2.55, 0])
        cap = _text(beat.get("caption", ""), 26, T.TEXT_DIM)
        cap.next_to(frame, DOWN, buff=0.22)
        shot = Group(frame, img, dots, url)
        g = Group(head, shot, cap)
        g.enter_anim = AnimationGroup(FadeIn(head, shift=UP * 0.3),
                                      FadeIn(shot, shift=UP * 0.3, scale=0.96),
                                      FadeIn(cap, shift=UP * 0.2),
                                      lag_ratio=0.3)
        foc = beat.get("focus")
        if foc:
            fx, fy, fw, fh = foc
            x0 = img.get_left()[0] + fx * img.width
            y0 = img.get_top()[1] - fy * img.height
            box = RoundedRectangle(corner_radius=0.08, width=fw * img.width,
                                   height=fh * img.height,
                                   stroke_color=T.ACCENT_STRONG,
                                   stroke_width=5, fill_opacity=0)
            box.move_to([x0 + fw * img.width / 2, y0 - fh * img.height / 2, 0])

            def post():
                self.play(Create(box), run_time=0.6)
                shot.add(box)
            self._post.append(post)

        def hold(left):
            self.play(shot.animate.scale(1.045), run_time=left,
                      rate_func=rate_functions.linear)
        self._hold.append(hold)
        return g

    # ======================================================================
    # YouTube call to action + end screen
    # ======================================================================
    def _r_cta(self, beat):
        v = beat["variant"]
        if v == "like":
            th = thumbs_up(1.25, T.TEXT_MUTED).move_to([0, 0.75, 0])
            t = _text("Like the video", 56, T.TEXT, bold=True)
            s = _text("it helps the channel and tells us what to make next",
                      28, T.TEXT_DIM)
            VGroup(t, s).arrange(DOWN, buff=0.3).move_to([0, -1.75, 0])
            plus = _text("+1", 40, T.ACCENT, bold=True).move_to([1.6, 1.4, 0])
            plus.set_opacity(0)
            g = VGroup(th, t, s, plus)
            g.enter_anim = AnimationGroup(FadeIn(th, scale=0.4),
                                          FadeIn(t, shift=UP * 0.3),
                                          FadeIn(s, shift=UP * 0.2),
                                          lag_ratio=0.3)

            def post():
                self.play(th.animate.set_fill(T.ACCENT).scale(1.18).rotate(0.12),
                          run_time=0.3)
                self.play(th.animate.scale(1 / 1.18).rotate(-0.12),
                          plus.animate.set_opacity(1).shift(UP * 0.5),
                          run_time=0.35)
                self.play(plus.animate.set_opacity(0).shift(UP * 0.4),
                          run_time=0.5)
            self._post.append(post)
            return g
        if v == "subscribe":
            av = logo(0.62)
            ch = VGroup(_text("Proto Vision Lab", 34, T.TEXT, bold=True),
                        _text("open-source hardware & firmware", 22,
                              T.TEXT_MUTED)).arrange(DOWN, buff=0.08,
                                                     aligned_edge=LEFT)
            btn = RoundedRectangle(corner_radius=0.34, width=3.1, height=0.82,
                                   fill_color=T.YT_RED, fill_opacity=1,
                                   stroke_width=0)
            bt = _text("SUBSCRIBE", 30, "#ffffff", bold=True).move_to(btn)
            button = VGroup(btn, bt)
            row = VGroup(av, ch, button).arrange(RIGHT, buff=0.45)
            row.move_to([0, 0.55, 0])
            bl = bell(0.75, T.TEXT).next_to(button, RIGHT, buff=0.45)
            bl.set_opacity(0)
            tag = _pill("notifications on", T.ACCENT_STRONG, 22)
            tag.move_to([0, -1.05, 0])
            tag.set_opacity(0)
            cap = _text("Subscribe so you don't miss the next one", 34,
                        T.TEXT_DIM).move_to([0, -2.3, 0])
            cursor = _poly((0, 0), (0, -0.55), (0.14, -0.42), (0.26, -0.68),
                           (0.36, -0.63), (0.24, -0.38), (0.42, -0.38),
                           closed=True, color=T.BG, width=2, fill="#ffffff")
            cursor.move_to([4.5, -2.0, 0])
            cursor.set_opacity(0)
            g = VGroup(row, bl, tag, cap, cursor)
            g.enter_anim = AnimationGroup(FadeIn(VGroup(av, ch), shift=UP * 0.2),
                                          FadeIn(button, scale=0.5),
                                          FadeIn(cap, shift=UP * 0.2),
                                          lag_ratio=0.3)

            def post():
                cursor.set_opacity(1)
                self.play(FadeIn(cursor), run_time=0.2)
                self.play(cursor.animate.move_to(btn.get_center()
                                                  + [0.25, -0.3, 0]),
                          run_time=0.7)
                new_bt = _text("SUBSCRIBED", 28, T.TEXT_DIM, bold=True)
                new_bt.move_to(btn)
                self.play(button.animate.scale(0.92), run_time=0.12)
                self.play(button.animate.scale(1 / 0.92),
                          btn.animate.set_fill(T.RAISED),
                          Transform(bt, new_bt), run_time=0.25)
                bl.set_opacity(1)
                self.play(FadeIn(bl, scale=0.5), run_time=0.3)
                self.play(cursor.animate.move_to(bl.get_center()
                                                 + [0.2, -0.3, 0]),
                          run_time=0.5)
                piv = bl.get_top()
                self.play(Succession(
                    Rotate(bl, 0.45, about_point=piv, run_time=0.15),
                    Rotate(bl, -0.9, about_point=piv, run_time=0.25),
                    Rotate(bl, 0.7, about_point=piv, run_time=0.2),
                    Rotate(bl, -0.25, about_point=piv, run_time=0.15)),
                    bl.animate.set_color(T.ACCENT_STRONG), run_time=0.75)
                tag.set_opacity(1)
                self.play(FadeIn(tag, shift=UP * 0.2),
                          FadeOut(cursor), run_time=0.4)
            self._post.append(post)
            return g
        if v == "comment":
            msg = _text("Questions? Feature ideas?", 46, T.TEXT, bold=True)
            bub = RoundedRectangle(corner_radius=0.35, width=msg.width + 1.2,
                                   height=2.0,
                                   fill_color=T.CARD, fill_opacity=1,
                                   stroke_color=T.ACCENT, stroke_width=3)
            bub.move_to([0, 0.75, 0])
            tail = _poly((-2.4, bub.get_bottom()[1] + 0.02),
                         (-3.0, bub.get_bottom()[1] - 0.6),
                         (-1.6, bub.get_bottom()[1] + 0.02), closed=True,
                         color=T.ACCENT, width=3, fill=T.CARD)
            cover = Line([-2.36, bub.get_bottom()[1], 0],
                         [-1.64, bub.get_bottom()[1], 0], stroke_color=T.CARD,
                         stroke_width=5)
            msg.move_to(bub)
            s = _text("Leave a comment below. We read every one.", 32,
                      T.TEXT_DIM).move_to([0, -1.55, 0])
            g = VGroup(tail, bub, cover, msg, s)
            g.enter_anim = AnimationGroup(FadeIn(VGroup(tail, bub, cover),
                                                 scale=0.6), lag_ratio=0.3)
            msg.set_opacity(0)

            def post():
                msg.set_opacity(1)
                self.play(AddTextLetterByLetter(msg, time_per_char=0.04),
                          run_time=1.0)
                self.play(FadeIn(s, shift=UP * 0.2), run_time=0.4)
            self._post.append(post)
            return g
        raise ValueError(v)

    def _r_endscreen(self, beat):
        t = _text("Thanks for watching", 64, T.TEXT, bold=True)
        s = _text("see you in the next video", 30, T.TEXT_DIM)
        hdr = VGroup(t, s).arrange(DOWN, buff=0.22).move_to([0, 2.0, 0])
        # Reserved areas for YouTube end-screen elements (added in Studio)
        vid = RoundedRectangle(corner_radius=0.15, width=5.6, height=3.15,
                               stroke_color=T.BORDER, stroke_width=2.5,
                               fill_color=T.PANEL, fill_opacity=1)
        vid.move_to([2.6, -1.25, 0])
        vlab = _text("Next video", 26, T.TEXT_MUTED).move_to(vid)
        sub = Circle(radius=1.2, stroke_color=T.BORDER, stroke_width=2.5,
                     fill_color=T.PANEL, fill_opacity=1).move_to([-3.4, -1.25, 0])
        mark = logo(0.9).move_to(sub)
        g = VGroup(hdr, VGroup(sub, mark), VGroup(vid, vlab))
        g.enter_anim = AnimationGroup(FadeIn(hdr, shift=UP * 0.3),
                                      FadeIn(g[1], scale=0.8),
                                      FadeIn(g[2], scale=0.9), lag_ratio=0.3)
        return g
