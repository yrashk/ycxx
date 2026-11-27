#!/usr/bin/env python3
"""Capture the exact #1647 example from the production binary."""
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
from playwright.sync_api import expect, sync_playwright
import pyte

DARWIN = "arm64-apple-darwin"
APP = Path(".ycxx/out/app")
FONT_DIR = Path("/usr/share/fonts/truetype/dejavu")
COLORS = {
    "black": "#232831", "red": "#f07882", "green": "#a3d68d", "brown": "#e7c277",
    "blue": "#80aaff", "magenta": "#d397f8", "cyan": "#77cbd6", "white": "#e5e9ef",
    "brightblack": "#747f92", "brightred": "#ff8f9e", "brightgreen": "#b4eaa0",
    "brightyellow": "#ffe29a", "brightblue": "#a6c4ff", "brightmagenta": "#e6aeff",
    "brightcyan": "#a7e5ee", "brightwhite": "#ffffff",
}


def command(driver, scope, env, *args):
    return subprocess.run([driver, *args], cwd=scope, env=env, capture_output=True, timeout=120)


def build(driver, scope, env, output, label, *options):
    result = command(driver, scope, env, "build", "app", *options, "--trace-engine=json")
    (output / f"{label}.jsonl").write_bytes(result.stderr)
    assert result.returncode == 0, result.stderr[-8000:].decode(errors="replace")
    records = [json.loads(line) for line in result.stderr.splitlines() if line.strip()]
    assert records[-1]["status"] == 0, records[-1]
    return records


def show(driver, scope, env, output, label, *options):
    for kind in ("json", "html"):
        result = command(driver, scope, env, "show", "app", *options, f"--format={kind}")
        assert result.returncode == 0, result.stderr
        path = output / f"{label}.{kind}"
        path.write_bytes(result.stdout)
    return path


def score(scope):
    return subprocess.run([scope / APP], cwd=scope, capture_output=True, timeout=5).returncode


def render_graph(report, output):
    report_requests, page_errors = [], []
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(
            executable_path=shutil.which("chromium"),
            args=["--no-sandbox", "--disable-dev-shm-usage", "--disable-background-networking"],
        )
        try:
            context = browser.new_context(offline=True, service_workers="block",
                                          viewport={"width": 1500, "height": 900})
            context.route(re.compile(r"^https?://"), lambda route: route.abort())
            page = context.new_page()
            page.on("request", lambda request: report_requests.append(request.url))
            page.on("websocket", lambda socket: report_requests.append(socket.url))
            page.on("pageerror", lambda error: page_errors.append(str(error)))
            page.goto(report.as_uri(), wait_until="load")
            graph = page.locator("#target-graph")
            expect(graph).to_be_visible()
            for label in ("config", "scanner", "bridge"):
                node = graph.get_by_role("button", name=label, exact=True)
                node.click()
                expect(graph.locator("#graph-target")).to_have_text(label)
                expect(node).to_have_attribute("aria-pressed", "true")
                graph.get_by_role("button", name="Expand dependencies", exact=True).click()
            for label in ("app", "config", "scanner", "bridge", "host_impl", "app_impl"):
                expect(graph.get_by_role("button", name=label, exact=True)).to_have_count(1)
            detail = graph.locator("#graph-details")
            host_path = ".ycxx/host/virtual/host_impl/impl.cc"
            target_path = f".ycxx/targets/{DARWIN}/virtual/app_impl/impl.cc"
            for logical in (host_path, target_path):
                expect(detail).to_contain_text(logical)
            for role, label in (("host", "host_impl"), ("target", "app_impl")):
                occurrence = detail.locator(".graph-relation").filter(has_text=label)
                expect(occurrence).to_have_count(1)
                expect(occurrence).to_contain_text("sources")
                origin = occurrence.locator("a[href^='#manifest-line-']")
                expect(origin).to_have_count(1)
                expect(page.locator(origin.get_attribute("href"))).to_contain_text(
                    f'{role} = artifact("{label}", "source")')
            page.wait_for_function("""() => {
              const canvas = document.getElementById("graph-edges");
              const pixels = canvas.getContext("2d").getImageData(0, 0, canvas.width, canvas.height).data;
              for (let i = 3; i < pixels.length; i += 4) if (pixels[i]) return true;
              return false;
            }""")
            graph.locator("#graph-viewport").evaluate("view => { view.scrollLeft = 180; }")
            graph.screenshot(path=output / "foreign-graph.png", animations="disabled")
            detail.locator(".graph-fact").filter(has_text=host_path).first.scroll_into_view_if_needed()
            graph.screenshot(path=output / "foreign-graph-paths.png", animations="disabled")
            external_requests = [url for url in report_requests if re.match(r"^(https?|wss?)://", url)]
            assert not external_requests and not page_errors, (external_requests, page_errors)
            return {"browser_offline": True, "report_network_requests": len(external_requests),
                    "host_path": host_path, "target_path": target_path}
        finally:
            browser.close()


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

    def current_actions(lines, build_index):
        roots = [(int(match[1]), index) for index, line in enumerate(lines)
                 if (match := re.search(r"request\(build (\d+)", line.partition("│")[0]))]
        if not roots:
            return {}
        latest, current = max(roots)
        if latest != build_index:
            return {}
        next_root = min((index for _, index in roots if index > current), default=len(lines))
        actions = {}
        for row in range(current + 1, next_root):
            left = lines[row].partition("│")[0]
            match = re.search(r"ExternalAction\(([^)]*)\) executed", left)
            if match:
                actions[match[1]] = row + 1
        return actions

    def edit(file, value):
        preparation = file.with_name(file.name + ".next")
        preparation.write_text(f"{value}\n", encoding="ascii")
        preparation.replace(file)

    try:
        read_until(lambda lines: any("watching " in line for line in lines)
                   and any("last build:" in line for line in lines))
        assert score(scope) == 57
        set_filter("kind:ExternalAction")
        for build_index, role, value, expected, wanted in (
            (2, "host", 23, 63, {"host_impl", "config"}),
            (3, "target", 45, 68, {"app_impl"}),
        ):
            if any("paused, f follows" in line for line in screen.display):
                os.write(terminal, b"f")
            edit(scope / f"{role}_score.txt", value)
            read_until(lambda lines: current_actions(lines, build_index).keys() == wanted and score(scope) == expected)
            action = "host_impl" if role == "host" else "app_impl"
            row = current_actions(screen.display, build_index)[action]
            os.write(terminal, f"\x1b[<0;9;{row}M\x1b[<0;9;{row}m".encode())
            read_until(lambda lines: any(line.partition("│")[2].strip() == action for line in lines))
            save_frame(screen, output / f"watch-{role}")
            print(f"{role} edit: app exit {expected}, actions {', '.join(sorted(wanted))}", flush=True)
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
    driver, example, output = [Path(argument).resolve() for argument in sys.argv[1:]]
    output.mkdir(parents=True, exist_ok=True)
    with (
        tempfile.TemporaryDirectory(prefix="ycxx-role-example-") as checkout,
        tempfile.TemporaryDirectory(prefix="y1647-", dir="/tmp") as runtime,
    ):
        scope = Path(checkout) / "example"
        shutil.copytree(example, scope, ignore=shutil.ignore_patterns(".ycxx", ".git"))
        (scope / ".git").mkdir()
        env = {key: value for key, value in os.environ.items() if key not in {
            "YCXX_NO_DAEMON", "YCXX_NO_WATCH", "YCXX_ROOT", "YCXX_TRACE_ENGINE", "NO_COLOR",
        }}
        env.update(YCXX_RUNTIME_DIR=runtime, YCXX_DAEMON="require", YCXX_DAEMON_IDLE_EXIT="120",
                   YCXX_THREADS="2", TERM="xterm-256color")
        try:
            foreign = f"--target={DARWIN}"
            cold = build(driver, scope, env, output, "foreign-build", foreign)
            effects = {row["key"] for row in cold if row.get("kind") == "ExternalAction"
                       and row.get("outcome") == "executed"}
            assert effects == {"host_impl", "app_impl", "config"}, effects
            artifact = scope / ".ycxx/targets" / DARWIN / "out/app"
            foreign_bytes = artifact.read_bytes()
            assert foreign_bytes[:4] == b"\xcf\xfa\xed\xfe"
            assert struct.unpack_from("<I", foreign_bytes, 4)[0] == 0x0100000C
            assert struct.unpack_from("<I", foreign_bytes, 12)[0] == 2
            (output / "foreign-image.txt").write_bytes(subprocess.check_output(["file", str(artifact)]))
            report = show(driver, scope, env, output, "foreign-plan", foreign)
            build(driver, scope, env, output, "default-build")
            assert score(scope) == 57
            print("initial app exit 57", flush=True)
            offline = render_graph(report, output)
            watch(driver, scope, env, output)
            assert score(scope) == 68 and artifact.read_bytes() == foreign_bytes
            show(driver, scope, env, output, "updated-plan")
            warm = build(driver, scope, env, output, "warm-build")
            assert warm[-1]["actions_executed"] == 0
            with driver.open("rb") as binary:
                sha = hashlib.file_digest(binary, "sha256").hexdigest()
            evidence = {"scores": [57, 63, 68], "darwin_image": "Mach-O arm64 executable",
                        "warm_actions": 0, "driver_sha256": sha, **offline}
            (output / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
        finally:
            command(driver, scope, env, "ctl", "shutdown")


if __name__ == "__main__":
    main()
