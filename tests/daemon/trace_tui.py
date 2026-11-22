#!/usr/bin/env python3
"""The interactive trace browser on a real terminal (#635): `ycxx cxx --trace-engine=interactive`
under a pseudo-terminal.

    tests/daemon/trace_tui.py <ycxx>

- It turns SGR mouse reports on (1000, 1002, 1006) and autowrap off, and back on `q`.
- A click on a tab, sent as the terminal would send it, switches the view; a drag moves the
  divider.
- A resize redraws at the new width.
- Copying (#636): `y` and a drag over rows put the text on the clipboard through OSC 52 and the
  local command (`YCXX_CLIPBOARD`, here a file).
- A fatal signal (SIGTERM, SIGABRT) gives the terminal back: mouse reports off, the main screen,
  cooked mode; then the process dies of that signal.

The model behind the frames is unit-tested (`unit.trace`); this checks the terminal around it.
"""
import base64, fcntl, os, re, select, signal, struct, sys, tempfile, termios, time
from pathlib import Path

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print(f'FAILED: {what}', file=sys.stderr)


SOURCE = 'int square(int x) { return x * x; }\nint main() { return square(3) - 9; }\n'
SGR = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]')


def set_size(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))


class Tui:
    """ycxx under a pty. The pty's session leader is a small parent that runs ycxx as its child, then
    writes how it ended and the terminal's modes then (`DONE status=… icanon=… echo=…`): a slave
    read by anyone else after the leader exits is revoked on macOS."""

    def __init__(self, ycxx, scope, env):
        self.fd, slave = os.openpty()
        set_size(self.fd, 30, 120)
        r, w = os.pipe()
        leader = os.fork()
        if leader == 0:
            os.close(self.fd)
            os.close(r)
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            for n in (0, 1, 2):
                os.dup2(slave, n)
            child = os.fork()
            if child == 0:
                os.chdir(scope)
                os.execve(ycxx, [ycxx, 'cxx', '--trace-engine=interactive', 'a.cpp', '-o', 'a'], env)
            os.write(w, str(child).encode())
            os.close(w)
            _, status = os.waitpid(child, 0)
            a = termios.tcgetattr(0)
            os.write(1, f'\r\nDONE status={status} icanon={int(bool(a[3] & termios.ICANON))} echo={int(bool(a[3] & termios.ECHO))}\r\n'.encode())
            os._exit(0)
        os.close(slave)
        os.close(w)
        self.leader = leader
        self.pid = int(os.read(r, 32))
        os.close(r)
        self.out = b''

    def read_until(self, pred, timeout=120):
        end = time.time() + timeout
        while time.time() < end:
            if pred(self.out):
                return True
            r, _, _ = select.select([self.fd], [], [], 0.2)
            if r:
                try:
                    chunk = os.read(self.fd, 65536)
                except OSError:
                    return pred(self.out)
                if not chunk:
                    return pred(self.out)
                self.out += chunk
        return pred(self.out)

    def send(self, data):
        os.write(self.fd, data)

    @staticmethod
    def frame_of(out):
        # Lines are drawn at their rows (`ESC [ row ; 1 H`); the last frame starts at row 1.
        text = out.decode('utf-8', 'replace')
        return re.split(r'\x1b\[\d+;1H', text[text.rfind('\x1b[1;1H'):])[1:]

    def last_frame(self):
        return Tui.frame_of(self.out)

    def wait(self, timeout=30):
        """ycxx's wait status, and whether the terminal was in cooked mode (ICANON and ECHO) after."""
        done = re.compile(rb'DONE status=(\d+) icanon=(\d) echo=(\d)')
        ok = self.read_until(lambda o: done.search(o) is not None, timeout)
        os.waitpid(self.leader, 0) if ok else os.kill(self.leader, signal.SIGKILL)
        m = done.search(self.out)
        if not m:
            return -1, False
        return int(m[1]), m[2] == b'1' and m[3] == b'1'


def plain_lines(frame):
    return [SGR.sub('', l) for l in frame]


def main(ycxx):
    scope = Path(tempfile.mkdtemp(prefix='ycxx-tui-'))
    (scope / 'a.cpp').write_text(SOURCE)
    env = {k: v for k, v in os.environ.items() if k not in ('NO_COLOR', 'YCXX_DAEMON')}
    clip = scope / 'clip.txt'
    env.update(YCXX_NO_DAEMON='1', TERM='xterm-256color', YCXX_CLIPBOARD=f'cat > {clip}')

    t = Tui(ycxx, scope, env)
    check(t.read_until(lambda o: b'ycxx engine trace' in o), 'the browser draws')
    check(b'\x1b[?7l' in t.out and b'\x1b[?1000h' in t.out and b'\x1b[?1002h' in t.out and b'\x1b[?1006h' in t.out, 'mouse reports on: clicks, drags, SGR')
    check(b'cell\xe2\x86\x93' in t.out, 'the tree, with its column headers')
    # Where the tabs are drawn (the summary above may wrap): click the timeline's, 1-based.
    t.read_until(lambda o: False, timeout=0.5)
    lines = plain_lines(t.last_frame())
    row = next((i for i, l in enumerate(lines) if l.startswith(' tree  timeline')), None)
    check(row is not None, f'the status line is drawn: {lines[:4]}')
    row = (row or 1) + 1
    t.send(f'\x1b[<0;10;{row}M'.encode())
    t.send(f'\x1b[<0;10;{row}m'.encode())
    check(t.read_until(lambda o: b'scrub' in o), 'a click on the timeline tab shows the timeline')
    t.send(f'\x1b[<0;3;{row}M\x1b[<0;3;{row}m'.encode())
    check(t.read_until(lambda o: o.rfind(b'cell\xe2\x86\x93') > o.rfind(b'scrub')), 'a click on the tree tab shows the tree')
    # Escape and a wheel report in one read (Escape during momentum scrolling) stay two inputs: the
    # report's bytes are no keys (`6` would pick worker 6).
    t.send(b'\x1b\x1b[<65;40;10M')
    t.read_until(lambda o: False, timeout=0.7)
    check(any('worker: all' in l for l in plain_lines(t.last_frame())), 'Escape then a mouse report: no stray keys')
    # A legacy report (a terminal without 1006): a click on the timeline's tab, then its release.
    t.send(b'\x1b[M' + bytes([32, 33 + 9, 32 + row]) + b'\x1b[M' + bytes([35, 33 + 9, 32 + row]))
    check(t.read_until(lambda o: o.rfind(b'scrub') > o.rfind(b'cell\xe2\x86\x93')), 'a legacy click on the timeline tab shows the timeline')
    t.send(f'\x1b[<0;3;{row}M\x1b[<0;3;{row}m'.encode())
    check(t.read_until(lambda o: o.rfind(b'cell\xe2\x86\x93') > o.rfind(b'scrub')), 'and back to the tree')
    # A drag of the divider (marked ⇔ in the column headers' row) moves it.
    t.read_until(lambda o: False, timeout=0.5)
    lines = plain_lines(t.last_frame())
    hdr = next((i for i, l in enumerate(lines) if l.startswith(' cell')), None)
    at = lines[hdr].find('⇔') if hdr is not None else -1
    check(at > 40, f'the divider is drawn: {lines[hdr] if hdr is not None else lines[:5]}')
    if at > 40:
        y = hdr + 1
        t.send(f'\x1b[<0;{at + 1};{y}M'.encode())
        for x in range(at, at - 21, -5):
            t.send(f'\x1b[<32;{x + 1};{y}M'.encode())
        t.send(f'\x1b[<0;{at - 19};{y}m'.encode())
        moved = lambda o: any(l.startswith(' cell') and l.find('⇔') == at - 20 for l in plain_lines(Tui.frame_of(o)))
        check(t.read_until(moved, timeout=10), f'a drag moves the divider from column {at} to {at - 20}: {[l for l in plain_lines(t.last_frame()) if l.startswith(" cell")]}')
    # Copying: y copies the selected cell (the first row, Build); a drag over three rows copies them.
    def osc52_last(o):
        m = re.findall(rb'\x1b\]52;c;([A-Za-z0-9+/=]*)\x07', o)
        return base64.b64decode(m[-1]).decode() if m else None
    t.send(b'g')
    t.send(b'y')
    check(t.read_until(lambda o: osc52_last(o) is not None, timeout=10), 'y writes OSC 52')
    copied = osc52_last(t.out) or ''
    check(copied.startswith('Build\n'), f'the cell: {copied[:40]!r}')
    check(t.read_until(lambda o: any('copied the cell (OSC 52, cat)' in l for l in plain_lines(Tui.frame_of(o))), timeout=5), 'the status line says how')
    check(clip.exists() and clip.read_text() == copied, 'and the same text through the clipboard command')
    lines = plain_lines(t.last_frame())
    first = next((i for i, l in enumerate(lines) if '● Build(' in l or 'Build(' in l), None)
    check(first is not None, 'the Build row is drawn')
    if first is not None:
        y = first + 1
        t.send(f'\x1b[<0;10;{y}M\x1b[<32;10;{y + 1}M\x1b[<32;10;{y + 2}M\x1b[<0;10;{y + 2}m'.encode())
        check(t.read_until(lambda o: (osc52_last(o) or '').count('\n') == 3, timeout=10), f'a drag over three rows copies three lines: {osc52_last(t.out)!r}')
        rows = osc52_last(t.out) or ''
        check(t.read_until(lambda o: any('copied 3 rows' in l for l in plain_lines(Tui.frame_of(o))), timeout=5), 'the status line says 3 rows')
        check(rows.startswith('* Build(') and clip.read_text() == rows, f'as the stream prints them: {rows[:60]!r}')
    # A resize redraws at once, at the new width. The last frame is read whole first, so what
    # follows the mark can only be a redraw.
    t.read_until(lambda o: False, timeout=1)
    mark = len(t.out)
    set_size(t.fd, 30, 140)
    check(t.read_until(lambda o: b'\x1b[30;1H' in o[mark:]), 'a resize redraws')
    time.sleep(0.3)
    t.read_until(lambda o: False, timeout=0.3)
    lines = plain_lines(t.last_frame())
    check(len(lines) == 30 and all(len(l) == 140 for l in lines), f'the new frame is 30×140: {len(lines)} lines, widths {sorted(set(len(l) for l in lines))}')
    t.send(b'q')
    status, _ = t.wait()
    check(os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, f'q quits (status {status})')
    tail = t.out[t.out.rfind(b'\x1b[?1006l'):] if b'\x1b[?1006l' in t.out else b''
    check(b'\x1b[?1000l' in tail and b'\x1b[?7h' in tail and b'\x1b[?1049l' in tail, 'mouse reports and autowrap back as they were, and the main screen')

    # A clipboard command that never reads holds the browser 2 s at most, and is killed with what it
    # started.
    marker = scope / 'stuck.txt'
    # A subshell outlives a kill of `sh` alone: only killing the process group stops its `touch`.
    t = Tui(ycxx, scope, dict(env, YCXX_CLIPBOARD=f'(sleep 3; touch {marker}) & wait'))
    check(t.read_until(lambda o: b'ycxx engine trace' in o), 'the browser draws (a stuck clipboard command)')
    began = time.time()
    t.send(b'Y')
    check(t.read_until(lambda o: any('copied the subtree' in l and '(OSC 52)' in l for l in plain_lines(Tui.frame_of(o))), timeout=10),
          'a command that never reads: the copy goes through OSC 52 alone')
    check(time.time() - began < 5, f'and the browser waited at most about 2 s ({time.time() - began:.1f} s)')
    t.send(b'q')
    t.wait()
    time.sleep(3)
    check(not marker.exists(), 'the command was killed, with what it started')

    for sig in (signal.SIGTERM, signal.SIGABRT):
        t = Tui(ycxx, scope, env)
        check(t.read_until(lambda o: b'ycxx engine trace' in o), f'the browser draws ({sig.name})')
        mark = len(t.out)
        os.kill(t.pid, sig)
        status, cooked = t.wait()
        check(os.WIFSIGNALED(status) and os.WTERMSIG(status) == sig, f'{sig.name} still ends the process by that signal (status {status})')
        after = t.out[mark:]
        check(b'\x1b[?1006l' in after and b'\x1b[?1000l' in after and b'\x1b[?1049l' in after, f'{sig.name} gives the terminal back: {after[-80:]!r}')
        check(cooked, f'{sig.name} restores cooked mode')

    if failures:
        print(f'daemon.trace_tui: {len(failures)} check(s) failed', file=sys.stderr)
        return 1
    print('daemon.trace_tui: all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main(os.path.abspath(sys.argv[1])))
