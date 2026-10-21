"""Turn the raw dashboard recording into the published artefacts with ffmpeg:

  docs/demo.mp4          title card -> speed-normalised run -> end card (H.264 1080p, < 25 MB)
  docs/demo.gif          720p-wide highlight of the fault + the rejected attack (< 10 MB)
  docs/img/thumbnail.png a strong frame for a portfolio card

The raw recording runs at the firmware's wall-clock rate (QEMU is not real time
on every host); recording.json carries the measured firmware/wall ratio, so the
run is sped up by 1/ratio to look real time, and the step markers map to times
in the final cut.

    python scripts/postprocess_video.py <raw.webm> <recording.json>
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DOCS = REPO / "docs"
FRONTEND = REPO / "dashboard" / "frontend"

TITLE_DUR, END_DUR, XF = 4.0, 5.0, 0.6  # card seconds, crossfade seconds
TARGET_MAIN = 82.0                       # seconds for the main run (cards add ~8 s)


def ffmpeg_exe() -> str:
    import imageio_ffmpeg
    return imageio_ffmpeg.get_ffmpeg_exe()


def run(args: list[str]) -> None:
    proc = subprocess.run([ffmpeg_exe(), "-y", "-hide_banner", "-loglevel", "error", *args],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr[-3000:] + "\n")
        raise RuntimeError("ffmpeg failed: " + " ".join(args[:6]) + " ...")


def duration(path: Path) -> float:
    import imageio_ffmpeg
    exe = imageio_ffmpeg.get_ffmpeg_exe()
    out = subprocess.run([exe, "-i", str(path)], capture_output=True, text=True).stderr
    for line in out.splitlines():
        if "Duration:" in line:
            h, m, s = line.split("Duration:")[1].split(",")[0].strip().split(":")
            return int(h) * 3600 + int(m) * 60 + float(s)
    return 0.0


def render_card(name: str, out_png: Path) -> None:
    from playwright.sync_api import sync_playwright
    url = (FRONTEND / name).resolve().as_uri()
    with sync_playwright() as pw:
        b = pw.chromium.launch(args=["--force-color-profile=srgb", "--hide-scrollbars"])
        pg = b.new_page(viewport={"width": 1920, "height": 1080})
        pg.goto(url)
        pg.wait_for_timeout(500)
        pg.screenshot(path=str(out_png))
        b.close()


def card_clip(png: Path, seconds: float, out: Path, br: str) -> None:
    run(["-loop", "1", "-framerate", "30", "-t", f"{seconds}", "-i", str(png),
         "-vf", f"fade=t=in:st=0:d=0.5,fade=t=out:st={seconds-0.5}:d=0.5,format=yuv420p",
         "-r", "30", "-c:v", "libx264", "-b:v", br, "-pix_fmt", "yuv420p", str(out)])


def encode(inp: list[str], vf: str, out: Path, br: str, extra: list[str] | None = None) -> None:
    run([*inp, "-vf", vf, "-an", "-r", "30", "-c:v", "libx264", "-b:v", br, "-maxrate",
         _mul(br, 1.4), "-bufsize", _mul(br, 2), "-pix_fmt", "yuv420p", *(extra or []), str(out)])


def _mul(br: str, k: float) -> str:
    return f"{int(int(br.rstrip('k')) * k)}k"


def postprocess(recording_json: Path) -> dict:
    recording_json = Path(recording_json)
    info = json.loads(recording_json.read_text())
    ratio = float(info.get("ratio", 0.8))
    fps = float(info.get("capture_fps", 12.0))
    frames_dir = Path(info["frames_dir"])
    marks = {label: float(t) for label, t in info.get("markers", [])}
    start = marks.get("scenario_start", 0.0)
    end = marks.get("scenario_end", info.get("wall_s", 0.0))
    work = recording_json.parent
    DOCS.mkdir(exist_ok=True)
    (DOCS / "img").mkdir(exist_ok=True)

    start_frame = max(0, int(start * fps))
    span = max(end - start, 1.0)                 # captured seconds of the run
    main_fw = min(TARGET_MAIN, span * ratio)     # final run length (never slower than real time)
    pts = main_fw / span                         # setpts factor to reach it
    main_br = f"{max(2500, int(21_000_000 * 8 / max(main_fw, 1) / 1000 / 8))}k"  # ~21 MB budget
    # assemble the captured frames, trim to the run, and speed to TARGET_MAIN
    # (QEMU runs slower than real time here; firmware-measured latencies in the
    # captions are unaffected - only the playback speed is normalised).
    main = work / "main.mp4"
    encode(["-framerate", f"{fps:.4f}", "-start_number", str(start_frame), "-i", str(frames_dir / "f%06d.jpg")],
           f"trim=end={span:.3f},setpts={pts:.5f}*PTS,fps=30,format=yuv420p", main, main_br)

    # title / end cards -> clips
    title_png, end_png = work / "title.png", work / "end.png"
    render_card("title.html", title_png)
    render_card("end.html", end_png)
    title_mp4, end_mp4 = work / "title.mp4", work / "end.mp4"
    card_clip(title_png, TITLE_DUR, title_mp4, main_br)
    card_clip(end_png, END_DUR, end_mp4, main_br)

    # concat title -> main -> end with crossfades
    demo_mp4 = DOCS / "demo.mp4"
    off1 = TITLE_DUR - XF
    off2 = TITLE_DUR + main_fw - 2 * XF
    fc = (f"[0:v][1:v]xfade=transition=fade:duration={XF}:offset={off1:.3f}[a];"
          f"[a][2:v]xfade=transition=fade:duration={XF}:offset={off2:.3f},format=yuv420p[v]")
    run(["-i", str(title_mp4), "-i", str(main), "-i", str(end_mp4), "-filter_complex", fc,
         "-map", "[v]", "-r", "30", "-c:v", "libx264", "-b:v", main_br, "-maxrate", _mul(main_br, 1.3),
         "-bufsize", _mul(main_br, 2), "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(demo_mp4)])

    # highlight GIF: the fault segment + the rejected-attack segment, cut from
    # `main` (times in the final cut) and stitched in one filter graph.
    def t_in_main(label: str, default: float) -> float:
        return max(0.0, (marks.get(label, start + default / ratio) - start) * pts)

    fa = t_in_main("fault", 20)
    at = t_in_main("attack", 55)
    demo_gif = DOCS / "demo.gif"
    _highlight_gif(main, demo_gif, (max(0, fa - 1.0), 5.5), (max(0, at - 0.8), 5.0), fps=12, width=720)
    if demo_gif.stat().st_size > 10_000_000:
        _highlight_gif(main, demo_gif, (max(0, fa - 1.0), 5.0), (max(0, at - 0.8), 4.5), fps=10, width=600)

    # thumbnail: a frame a moment after the fault (FAULT state on screen)
    thumb = DOCS / "img" / "thumbnail.png"
    run(["-ss", f"{fa + 2.2:.2f}", "-i", str(main), "-frames:v", "1", str(thumb)])

    return {"demo.mp4": (demo_mp4, demo_mp4.stat().st_size),
            "demo.gif": (demo_gif, demo_gif.stat().st_size),
            "thumbnail": (thumb, thumb.stat().st_size)}


def _highlight_gif(src: Path, out: Path, seg_a: tuple[float, float], seg_b: tuple[float, float],
                   fps: int, width: int) -> None:
    a0, ad = seg_a
    b0, bd = seg_b
    fc = (f"[0:v]trim=start={a0:.2f}:end={a0+ad:.2f},setpts=PTS-STARTPTS[a];"
          f"[0:v]trim=start={b0:.2f}:end={b0+bd:.2f},setpts=PTS-STARTPTS[b];"
          f"[a][b]concat=n=2:v=1[c];"
          f"[c]fps={fps},scale={width}:-1:flags=lanczos,split[s0][s1];"
          f"[s0]palettegen=max_colors=160[p];[s1][p]paletteuse=dither=bayer[o]")
    run(["-i", str(src), "-filter_complex", fc, "-map", "[o]", str(out)])


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: postprocess_video.py <recording.json>")
    outs = postprocess(Path(sys.argv[1]))
    for name, (path, size) in outs.items():
        print(f"{name:10s} {path}  ({size/1e6:.1f} MB)")
