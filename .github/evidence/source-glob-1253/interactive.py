#!/usr/bin/env python3
"""Capture interactive source glob watch in the native example (#1253)."""
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pty
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

from PIL import Image, ImageDraw, ImageFont
import pyte

FONT_DIR = Path("/usr/share/fonts/truetype/dejavu")
COLORS = {
    "black": "#232831", "red": "#f07882", "green": "#a3d68d", "brown": "#e7c277",
    "blue": "#80aaff", "magenta": "#d397f8", "cyan": "#77cbd6", "white": "#e5e9ef",
    "brightblack": "#747f92", "brightred": "#ff8f9e", "brightgreen": "#b4eaa0",
    "brightyellow": "#ffe29a", "brightblue": "#a6c4ff", "brightmagenta": "#e6aeff",
    "brightcyan": "#a7e5ee", "brightwhite": "#ffffff",
}


def save_frame(screen, path):
    fg, bg = "#e5e9ef", "#121620"
    image = Image.new("RGB", (screen.columns * 9 + 32, screen.lines * 20 + 28), bg)
    draw = ImageDraw.Draw(image)
    normal = ImageFont.truetype(str(FONT_DIR / "DejaVuSansMono.ttf"), 14)
    bold = ImageFont.truetype(str(FONT_DIR / "DejaVuSansMono-Bold.ttf"), 14)

    def color(name, default):
        if name in COLORS:
            return COLORS[name]
        return "#" + name if re.fullmatch("[0-9a-fA-F]{6}", name) else default

    for row in range(screen.lines):
        for column in range(screen.columns):
            cell = screen.buffer[row][column]
            foreground, background = color(cell.fg, fg), color(cell.bg, bg)
            if cell.reverse:
                foreground, background = background, foreground
            x, y = 16 + column * 9, 14 + row * 20
            if background != bg:
                draw.rectangle((x, y, x + 8, y + 19), fill=background)
            if cell.data.strip():
                draw.text((x, y + 2), cell.data, font=bold if cell.bold else normal, fill=foreground)
    image.save(path.with_suffix(".png"))
    path.with_suffix(".txt").write_text("\n".join(screen.display) + "\n", encoding="utf-8")


def watch(driver, scope, env, output):
    rows, columns = 34, 172
    child, terminal = pty.fork()
    if child == 0:
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        os.chdir(scope)
        os.execve(driver, [str(driver), "build", "app", "--watch", "--trace-engine=interactive"], env)
    screen = pyte.Screen(columns, rows)
    stream = pyte.ByteStream(screen)
    transcript = bytearray()
    active = ""

    def read_until(predicate):
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            ready, _, _ = select.select([terminal], [], [], .15)
            if ready:
                try:
                    chunk = os.read(terminal, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        break
                    raise
                if not chunk:
                    break
                transcript.extend(chunk)
                stream.feed(chunk)
            elif predicate(screen.display):
                return
        raise AssertionError("\n".join(screen.display))

    def set_filter(name):
        nonlocal active
        os.write(terminal, b"/" + b"\x7f" * len(active) + name.encode() + b"\r")
        active = name
        read_until(lambda lines: any(f"filter: {active}" in line for line in lines))

    def latest_completed(lines):
        return max((int(m[1]) for line in lines
                    if (m := re.search(r"request\(build (\d+)[^)]*\) (executed|reused|verified|cutoff)",
                                       line.partition("│")[0]))), default=0)

    def added_row(lines):
        return next((i for i, line in enumerate(lines, 1)
                     if "live_glob_probe.cc)" in line.partition("│")[0]
                     and "executed" in line.partition("│")[0]), 0)

    def score():
        return subprocess.run([scope / ".ycxx/out/app"], cwd=scope, capture_output=True, timeout=5).returncode

    try:
        read_until(lambda lines: any("watching " in line for line in lines)
                   and any("last build:" in line for line in lines) and latest_completed(lines))
        assert score() == 42
        set_filter("kind:Tu live_glob_probe.cc is:executed")
        generated = scope / ".ycxx/glob-probe-next"
        generated.write_text("int live_glob_probe() { return 41; }\n", encoding="ascii")
        source = scope / "src/live_glob_probe.cc"
        generated.replace(source)
        read_until(lambda lines: added_row(lines) and latest_completed(lines) >= 2 and score() == 42)
        row = added_row(screen.display)
        add_build = latest_completed(screen.display)
        os.write(terminal, f"\x1b[<0;9;{row}M\x1b[<0;9;{row}m".encode())
        read_until(lambda lines: any("live_glob_probe.cc" in line.partition("│")[2] for line in lines))
        save_frame(screen, output / "watch-added-source")

        set_filter("")
        if any("paused, f follows" in line for line in screen.display):
            os.write(terminal, b"f")
        source.unlink()
        read_until(lambda lines: latest_completed(lines) > add_build and score() == 42)
        remove_build = latest_completed(screen.display)
        save_frame(screen, output / "watch-removed-source")
        proof = {"addition_build": add_build, "removal_build": remove_build, "app_exit": 42}
        (output / "interactive-proof.json").write_text(json.dumps(proof, indent=2) + "\n")
        print("Interactive watch exercised addition and removal", proof, flush=True)
    finally:
        try:
            os.write(terminal, b"q")
        except OSError:
            pass
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            done, status = os.waitpid(child, os.WNOHANG)
            if done:
                break
            time.sleep(.05)
        else:
            os.kill(child, signal.SIGKILL)
            _, status = os.waitpid(child, 0)
        os.close(terminal)
        (output / "watch.ansi").write_bytes(transcript)
    assert os.waitstatus_to_exitcode(status) == 0, status


def main():
    driver, repo, out = (Path(arg).resolve() for arg in sys.argv[1:])
    out.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(repo / "tests/daemon"))
    from project_build import env_for, run_client
    from runtime_dir import runtime_root

    with (tempfile.TemporaryDirectory(prefix="ycxx-glob-live-") as workspace,
          tempfile.TemporaryDirectory(prefix="yb", dir=runtime_root("yb")) as runtime):
        scope = Path(workspace).resolve() / "example"
        shutil.copytree(repo / "examples/project-build/native", scope)
        for name in ("twice_test.cc", "glob_probe_test.cc"):
            (scope / "src" / name).write_text("#error excluded source compiled\n", encoding="ascii")
        env = env_for(runtime)
        env["TERM"] = "xterm-256color"
        env.pop("NO_COLOR", None)
        try:
            watch(driver, scope, env, out)
        finally:
            run_client(driver, scope, env, "ctl", "shutdown")


if __name__ == "__main__":
    main()
