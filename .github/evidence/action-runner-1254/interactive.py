#!/usr/bin/env python3
"""Capture actual watch events for sibling receipt validation (#1254)."""
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

    def row_for(lines, key, outcome, after=0):
        request = 0
        for i, line in enumerate(lines, 1):
            left = line.partition("│")[0]
            if (match := re.search(r"request\(build (\d+)", left)):
                request = int(match[1])
            if request >= after and key in left and outcome in left:
                return i
        return 0

    def select_row(row, label):
        os.write(terminal, f"\x1b[<0;9;{row}M\x1b[<0;9;{row}m".encode())
        read_until(lambda lines: any(label in line.partition("│")[2] for line in lines))

    def score():
        return subprocess.run([scope / ".ycxx/out/app"], cwd=scope, capture_output=True, timeout=5).returncode

    def edit(value):
        pending = scope / ".ycxx/watch-schema-next"
        pending.write_text(value + "\n", encoding="ascii")
        pending.replace(scope / "schema.txt")

    def snapshot():
        items = (".ycxx/out/app", ".ycxx/assets/app/wire.cc", ".ycxx/assets/app/wire.h")
        return {name: hashlib.sha256((scope / name).read_bytes()).hexdigest() for name in items}

    try:
        read_until(lambda lines: any("watching " in line for line in lines)
                   and any("last build:" in line for line in lines))
        assert score() == 17
        set_filter("kind:Tu")
        edit("11")
        read_until(lambda lines: row_for(lines, "Tu(wire.cc)", "executed", after=2) and score() == 21
                   and any("request(build 2" in line.partition("│")[0] for line in lines))
        select_row(row_for(screen.display, "Tu(wire.cc)", "executed", after=2), "wire.cc")
        save_frame(screen, output / "watch-source-edit")
        accepted = snapshot()

        set_filter("kind:ExternalAction wire")
        if any("paused, f follows" in line for line in screen.display):
            os.write(terminal, b"f")
        edit("rejected")
        read_until(lambda lines: row_for(lines, "ExternalAction(wire)", "failed"))
        assert score() == 21 and snapshot() == accepted
        select_row(row_for(screen.display, "ExternalAction(wire)", "failed"), "wire")
        save_frame(screen, output / "watch-failed-action")
        proof = {"exit_after_source_edit": 21, "exit_after_failure": 21,
                 "published_sha256_preserved": accepted}
        (output / "interactive-proof.json").write_text(json.dumps(proof, indent=2) + "\n")
        print("Interactive watch verified changed generated source and preserved previous outputs after failure")
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
    from project_build import env_for, prepare, run_client
    from runtime_dir import runtime_root

    with (tempfile.TemporaryDirectory(prefix="ycxx-receipt-live-") as workspace,
          tempfile.TemporaryDirectory(prefix="yb", dir=runtime_root("yb")) as runtime):
        scope = Path(workspace).resolve() / "example"
        scope.mkdir()
        prepare(scope)
        manifest = scope / "build.ycxx"
        definition = manifest.read_text()
        anchor = '    deps = ["core"]'
        assert definition.count(anchor) == 1
        manifest.write_text(definition.replace(anchor,
            '    runtime_assets = [artifact("wire", "source"), artifact("wire", "header")]\n' + anchor))
        env = env_for(runtime)
        env["TERM"] = "xterm-256color"
        env.pop("NO_COLOR", None)
        try:
            watch(driver, scope, env, out)
        finally:
            run_client(driver, scope, env, "ctl", "shutdown")


if __name__ == "__main__":
    main()
