"""Give only this test process's native windows focus inside an X11 test display."""

import os
from pathlib import Path
import subprocess
import sys
import time


def main():
    environment = dict(os.environ, SDL_VIDEODRIVER="x11", SDL_AUDIODRIVER="dummy",
                       SDL_RENDER_DRIVER="software", SNES_TEST_WINDOW="1")
    process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], env=environment)
    focused = set()
    deadline = time.monotonic() + 90
    try:
        while process.poll() is None:
            if time.monotonic() > deadline:
                raise TimeoutError("The native application checks did not finish within 90 seconds")
            windows = subprocess.run(
                ["xdotool", "search", "--onlyvisible", "--pid", str(process.pid)],
                capture_output=True, text=True, timeout=2)
            visible = set(windows.stdout.splitlines())
            focused.intersection_update(visible)
            for window in visible:
                if window not in focused:
                    result = subprocess.run(["xdotool", "windowfocus", "--sync", window],
                                            capture_output=True, timeout=2)
                    if result.returncode == 0:
                        focused.add(window)
            time.sleep(.02)
        return process.returncode
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
