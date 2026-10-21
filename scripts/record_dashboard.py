"""Record the dashboard by screenshotting it at a fixed rate with headless
Chromium (Playwright), in a background thread so the demo scenario drives the
page meanwhile.

Playwright's built-in video recording throttles to ~1 fps for a headless
"backgrounded" page, so instead we grab JPEG frames in a tight loop at a
target rate; postprocess_video.py assembles them (and speed-normalises). This
is deterministic and independent of Chromium's compositor throttling.
"""
from __future__ import annotations

import shutil
import threading
import time
from pathlib import Path

FLAGS = ["--force-color-profile=srgb", "--hide-scrollbars",
         "--disable-background-timer-throttling", "--disable-renderer-backgrounding",
         "--disable-backgrounding-occluded-windows",
         "--disable-features=CalculateNativeWinOcclusion,IntensiveWakeUpThrottling"]


class Recorder:
    def __init__(self, url: str, frames_dir: Path, target_fps: int = 12,
                 width: int = 1920, height: int = 1080) -> None:
        self.url = url
        self.frames_dir = Path(frames_dir)
        self.fps = target_fps
        self.size = {"width": width, "height": height}
        self._stop = threading.Event()
        self._started = threading.Event()
        self._thread: threading.Thread | None = None
        self._result: dict = {}
        self.start_wall = 0.0

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name="recorder", daemon=True)
        self._thread.start()
        if not self._started.wait(timeout=30):
            raise RuntimeError("recorder did not start")

    def _run(self) -> None:
        from playwright.sync_api import sync_playwright

        if self.frames_dir.exists():
            shutil.rmtree(self.frames_dir, ignore_errors=True)
        self.frames_dir.mkdir(parents=True, exist_ok=True)
        interval = 1.0 / self.fps
        stamps: list[float] = []
        with sync_playwright() as pw:
            browser = pw.chromium.launch(args=FLAGS)
            page = browser.new_page(viewport=self.size, device_scale_factor=1)
            page.goto(self.url)
            page.wait_for_timeout(500)
            self.start_wall = time.monotonic()
            self._started.set()
            n = 0
            while not self._stop.is_set():
                t = time.monotonic()
                try:
                    page.screenshot(path=str(self.frames_dir / f"f{n:06d}.jpg"), type="jpeg", quality=80)
                except Exception:
                    break
                stamps.append(t)
                n += 1
                nap = interval - (time.monotonic() - t)
                if nap > 0:
                    time.sleep(nap)
            browser.close()
        span = (stamps[-1] - stamps[0]) if len(stamps) > 1 else 0.0
        self._result = {"frames_dir": str(self.frames_dir), "count": n, "wall_s": span,
                        "capture_fps": (n - 1) / span if span > 0 else self.fps}

    def stop(self) -> dict:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=60)
        return self._result
