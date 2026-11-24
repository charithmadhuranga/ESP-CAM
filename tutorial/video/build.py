"""Build orchestrator for narration-synchronised tutorial videos.

Run from the project root (the folder holding ``video/``, the narration
markdowns and the mp3s — all paths resolve relative to this file):

    .venv/bin/python video/build.py diagram     # rebuild DrawCMS receipts
    .venv/bin/python video/build.py schedule    # validate sync schedules
    .venv/bin/python video/build.py render      # render all scenes (long!)
    .venv/bin/python video/build.py render ch1  # render one scene
    .venv/bin/python video/build.py mux         # attach narration audio
    .venv/bin/python video/build.py concat      # join into one video
    .venv/bin/python video/build.py all         # everything above

Rendered scenes land in ``manim_build/``, muxed chapters in ``final/``, and
the combined upload file in ``final/full.mp4``. Every step is idempotent:
finished outputs are skipped unless ``--force`` is given, so a failed run
can simply be restarted.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent  # project root
sys.path.insert(0, str(HERE))

from segments import ORDER, SEGMENTS  # noqa: E402
from timing import build_all, write_schedule  # noqa: E402

MANIM = ROOT / ".venv" / "bin" / "manim"
MEDIA = ROOT / "manim_build"
OUT = ROOT / "final"
SCHEDULE_PATH = ROOT / "schedule.json"
DRAWCMS_DIR = ROOT / ".drawcms"
SCENES = HERE / "scenes.py"
# The DrawCMS CLI that turns specs into build receipts; override with the
# DRAWCMS_CLI environment variable if it lives elsewhere.
DRAWCMS_SKILL = Path(
    os.environ.get("DRAWCMS_CLI",
                   str(Path.home() / ".agents/skills/drawcms/bin/drawcms.mjs"))
)

# 1920x1080 @ 30 fps, the format chosen for the YouTube upload.
RENDER_FLAGS = [
    "--fps", "30",
    "--resolution", "1920,1080",
    "--media_dir", str(MEDIA),
    "-v", "WARNING",
]


def scene_name(seg_id: str) -> str:
    if seg_id == "intro":
        return "Intro"
    if seg_id == "end":
        return "End"
    return f"Ch{int(seg_id[2:]):02d}"


def scene_media(seg_id: str) -> Path:
    # one media dir per scene: parallel renders must not share Manim's
    # text/SVG cache (they race on its temp files)
    return MEDIA / scene_name(seg_id)


def scene_video(seg_id: str) -> Path:
    return (scene_media(seg_id) / "videos" / "scenes" / "1080p30"
            / f"{scene_name(seg_id)}.mp4")


def muxed_video(seg_id: str) -> Path:
    idx = ORDER.index(seg_id) + 1
    return OUT / f"{idx:02d}-{seg_id}.mp4"


def resolve(ids: list[str]) -> list[str]:
    if not ids:
        return list(ORDER)
    unknown = [i for i in ids if i not in SEGMENTS]
    if unknown:
        sys.exit(f"unknown segment(s): {', '.join(unknown)}\n"
                 f"available: {', '.join(ORDER)}")
    return ids


def run(cmd: list[str], *, cwd: Path | None = None,
        capture: bool = False) -> subprocess.CompletedProcess:
    print(f"  $ {' '.join(str(c) for c in cmd)}")
    proc = subprocess.run(
        [str(c) for c in cmd], cwd=cwd, text=True,
        capture_output=capture,
    )
    if proc.returncode != 0:
        if capture:
            sys.stderr.write(proc.stdout or "")
            sys.stderr.write(proc.stderr or "")
        sys.exit(f"command failed (exit {proc.returncode})")
    return proc


# --------------------------------------------------------------------------
# steps
# --------------------------------------------------------------------------
def diagram_specs() -> list[Path]:
    """Every authored DrawCMS spec in ``.drawcms/`` (receipts excluded)."""
    return sorted(
        p for p in DRAWCMS_DIR.glob("*.json")
        if not p.name.endswith((".built.json", ".stash.json"))
        and p.name != "config.json"
    )


def cmd_diagram(_: argparse.Namespace) -> None:
    """Regenerate every DrawCMS build receipt Manim renders diagrams from."""
    if not DRAWCMS_SKILL.exists():
        sys.exit(f"DrawCMS CLI not found at {DRAWCMS_SKILL}")
    specs = diagram_specs()
    if not specs:
        sys.exit(f"no DrawCMS specs found in {DRAWCMS_DIR}")
    print(f"Rebuilding {len(specs)} DrawCMS diagram receipt(s) ...")
    for spec in specs:
        doc = json.loads(spec.read_text(encoding="utf-8"))
        dtype = doc.get("diagramType") or (doc.get("meta") or {}).get(
            "diagramType")
        if not dtype:
            sys.exit(f"{spec.name}: no diagramType found")
        proc = run(["node", DRAWCMS_SKILL, "build", dtype, spec, "--json"],
                   capture=True)
        try:
            receipt = json.loads(proc.stdout)
        except json.JSONDecodeError:
            sys.exit(f"DrawCMS returned non-JSON output:\n{proc.stdout[:2000]}")
        if not receipt.get("ok"):
            sys.exit(f"DrawCMS build failed for {spec.name}: "
                     f"{receipt.get('error')}")
        built = spec.with_name(f"{spec.stem}.built.json")
        built.write_text(json.dumps(receipt, indent=2), encoding="utf-8")
        summary = receipt.get("summary", {})
        print(f"  {spec.stem:<28} ok - {summary.get('nodes')} nodes, "
              f"{summary.get('edges')} edges, "
              f"{summary.get('storySteps')} story steps, "
              f"{len(receipt.get('warnings', []))} warnings")
        print(f"    wrote {built.relative_to(ROOT)}")


def build_schedules() -> dict:
    """Validate that every beat tiles its script; returns the schedule data."""
    segments = [SEGMENTS[i] for i in ORDER]
    return build_all(segments, ROOT)


def cmd_schedule(_: argparse.Namespace) -> None:
    data = build_schedules()
    write_schedule(data, SCHEDULE_PATH)
    print(f"{'seg':<7} {'audio':>8} {'beats':>6}  {'sync check':>10}")
    print("-" * 36)
    for seg in data["segments"]:
        print(f"{seg['id']:<7} {seg['audio_duration']:>7.2f}s "
              f"{len(seg['beats']):>6}  {'OK':>10}")
    total = data["total_audio"]
    print("-" * 36)
    print(f"{'TOTAL':<7} {total:>7.2f}s = {int(total // 60)}:{int(total % 60):02d}"
          f"   ({len(data['segments'])} segments)")
    print(f"wrote {SCHEDULE_PATH.relative_to(ROOT)}")


def cmd_render(args: argparse.Namespace) -> None:
    build_schedules()  # fail fast on a bad paragraph range
    ids = resolve(args.ids)
    todo = []
    for seg_id in ids:
        out = scene_video(seg_id)
        if out.exists() and not args.force:
            print(f"skip {seg_id} (already rendered: {out.relative_to(ROOT)})")
        else:
            todo.append(seg_id)
    if not todo:
        print("nothing to render")
        return
    if not MANIM.exists():
        sys.exit(f"manim not found at {MANIM} - create the venv first")
    jobs = max(1, int(getattr(args, "jobs", 1)))
    procs: list[tuple[str, subprocess.Popen]] = []
    failed = []
    LOGS = MEDIA / "logs"
    LOGS.mkdir(parents=True, exist_ok=True)

    def reap(block: bool) -> None:
        for item in list(procs):
            sid, pr = item
            if block or pr.poll() is not None:
                rc = pr.wait()
                procs.remove(item)
                if rc:
                    failed.append(sid)
                    print(f"  FAILED {sid} (see {LOGS / sid}.log)")
                else:
                    print(f"  -> {scene_video(sid).relative_to(ROOT)}")
    for n, seg_id in enumerate(todo, 1):
        while len(procs) >= jobs:
            reap(block=False)
            if len(procs) >= jobs:
                import time
                time.sleep(1)
        print(f"[{n}/{len(todo)}] rendering {seg_id} "
              f"({scene_name(seg_id)}) ...")
        flags = [f if f != str(MEDIA) else str(scene_media(seg_id))
                 for f in RENDER_FLAGS]
        log = open(LOGS / f"{seg_id}.log", "w")
        procs.append((seg_id, subprocess.Popen(
            [str(MANIM), "render", *flags, str(SCENES), scene_name(seg_id)],
            cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)))
    while procs:
        reap(block=True)
    if failed:
        sys.exit(f"render failed: {', '.join(failed)}")


def cmd_mux(args: argparse.Namespace) -> None:
    ids = resolve(args.ids)
    OUT.mkdir(parents=True, exist_ok=True)
    todo = []
    for seg_id in ids:
        out = muxed_video(seg_id)
        if out.exists() and not args.force:
            print(f"skip {seg_id} (already muxed: {out.name})")
        else:
            todo.append(seg_id)
    for seg_id in todo:
        video = scene_video(seg_id)
        if not video.exists():
            sys.exit(f"missing {video} - run `build.py render {seg_id}` first")
        audio = ROOT / SEGMENTS[seg_id]["audio"]
        out = muxed_video(seg_id)
        print(f"muxing {seg_id} <- {audio.name}")
        # The scene starts with a lead-in card (audio delayed to match) and
        # may end with an end-screen hold; the audio is padded with silence
        # to the exact video length.
        lead_ms = int(round(float(SEGMENTS[seg_id].get("lead_in", 0)) * 1000))
        vdur = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration",
             "-of", "csv=p=0", str(video)],
            capture_output=True, text=True, check=True).stdout.strip()
        run([
            "ffmpeg", "-y", "-i", video, "-i", audio,
            "-map", "0:v:0", "-map", "1:a:0",
            "-c:v", "copy",
            "-af", f"adelay={lead_ms}:all=1,apad",
            "-c:a", "aac", "-b:a", "192k", "-ar", "48000", "-ac", "2",
            "-movflags", "+faststart",
            "-t", vdur,
            out,
        ])
        print(f"  -> {out.relative_to(ROOT)}")


def cmd_concat(_: argparse.Namespace) -> None:
    missing = [i for i in ORDER if not muxed_video(i).exists()]
    if missing:
        sys.exit(f"missing chapters: {', '.join(missing)} - run `build.py mux`")
    OUT.mkdir(parents=True, exist_ok=True)
    list_path = OUT / "concat.txt"
    list_path.write_text(
        "\n".join(f"file '{muxed_video(i).name}'" for i in ORDER) + "\n",
        encoding="utf-8",
    )
    target = OUT / "full.mp4"
    print(f"concatenating {len(ORDER)} chapters ...")
    run([
        "ffmpeg", "-y", "-f", "concat", "-safe", "0", "-i", list_path,
        "-c", "copy", target,
    ], cwd=OUT)
    print(f"  -> {target.relative_to(ROOT)}")


def cmd_all(args: argparse.Namespace) -> None:
    cmd_diagram(args)
    cmd_schedule(args)
    cmd_render(args)
    cmd_mux(args)
    cmd_concat(args)


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="command", required=True)

    for name, fn, extra in [
        ("diagram", cmd_diagram, "rebuild the DrawCMS build receipt"),
        ("schedule", cmd_schedule, "validate narration sync schedules"),
        ("render", cmd_render, "render scene videos with Manim"),
        ("mux", cmd_mux, "attach narration audio to each scene"),
        ("concat", cmd_concat, "join all chapters into one video"),
        ("all", cmd_all, "diagram + schedule + render + mux + concat"),
    ]:
        p = sub.add_parser(name, help=extra)
        if name in ("render", "mux", "all"):
            p.add_argument("ids", nargs="*",
                           help="segment ids (default: all in order)")
            p.add_argument("--force", action="store_true",
                           help="rebuild even if the output exists")
            p.add_argument("-j", "--jobs", type=int, default=1,
                           help="render this many scenes in parallel")
        p.set_defaults(func=fn)

    args = parser.parse_args()
    args.ids = getattr(args, "ids", [])
    args.force = getattr(args, "force", False)
    args.func(args)


if __name__ == "__main__":
    main()
