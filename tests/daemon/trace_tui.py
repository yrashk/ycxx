#!/usr/bin/env python3
"""The interactive trace browser on a real terminal (#635): `ycxx cxx --trace-engine=interactive`
under a pseudo-terminal.

    tests/daemon/trace_tui.py <ycxx>

- It turns SGR mouse reports on (1000, 1002, 1006) and autowrap off, and back on `q`.
- A click on a tab, sent as the terminal would send it, switches the view.
- A resize redraws at the new width.
- A fatal signal (SIGTERM, SIGABRT) gives the terminal back: mouse reports off, the main screen,
  cooked mode; then the process dies of that signal.

The model behind the frames is unit-tested (`unit.trace`); this checks the terminal around it.
"""
import fcntl, os, pty, re, select, signal, struct, sys, tempfile, termios, time
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
    def __init__(self, ycxx, scope, env):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(scope)
            os.execve(ycxx, [ycxx, 'cxx', '--trace-engine=interactive', 'a.cpp', '-o', 'a'], env)
        set_size(self.fd, 30, 120)
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

    def last_frame(self):
        # Lines are drawn at their rows (`ESC [ row ; 1 H`); the last frame starts at row 1.
        text = self.out.decode('utf-8', 'replace')
        return re.split(r'\x1b\[\d+;1H', text[text.rfind('\x1b[1;1H'):])[1:]

    def wait(self, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            # Drain, so the child never blocks on a full pty.
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try:
                    self.out += os.read(self.fd, 65536)
                except OSError:
                    pass
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                return status
        os.kill(self.pid, signal.SIGKILL)
        return os.waitpid(self.pid, 0)[1]


def plain_lines(frame):
    return [SGR.sub('', l) for l in frame]


def main(ycxx):
    scope = Path(tempfile.mkdtemp(prefix='ycxx-tui-'))
    (scope / 'a.cpp').write_text(SOURCE)
    env = {k: v for k, v in os.environ.items() if k not in ('NO_COLOR', 'YCXX_DAEMON')}
    env.update(YCXX_NO_DAEMON='1', TERM='xterm-256color')

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
    status = t.wait()
    check(os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, f'q quits (status {status})')
    tail = t.out[t.out.rfind(b'\x1b[?1006l'):] if b'\x1b[?1006l' in t.out else b''
    check(b'\x1b[?1000l' in tail and b'\x1b[?7h' in tail and b'\x1b[?1049l' in tail, 'mouse reports and autowrap back as they were, and the main screen')

    for sig in (signal.SIGTERM, signal.SIGABRT):
        t = Tui(ycxx, scope, env)
        check(t.read_until(lambda o: b'ycxx engine trace' in o), f'the browser draws ({sig.name})')
        mark = len(t.out)
        os.kill(t.pid, sig)
        status = t.wait()
        check(os.WIFSIGNALED(status) and os.WTERMSIG(status) == sig, f'{sig.name} still ends the process by that signal (status {status})')
        after = t.out[mark:]
        check(b'\x1b[?1006l' in after and b'\x1b[?1000l' in after and b'\x1b[?1049l' in after, f'{sig.name} gives the terminal back: {after[-80:]!r}')
        try:
            attrs = termios.tcgetattr(t.fd)
            check(attrs[3] & termios.ICANON and attrs[3] & termios.ECHO, f'{sig.name} restores cooked mode')
        except termios.error:
            pass  # a master whose slave has closed has no attributes to read on every host

    if failures:
        print(f'daemon.trace_tui: {len(failures)} check(s) failed', file=sys.stderr)
        return 1
    print('daemon.trace_tui: all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main(os.path.abspath(sys.argv[1])))
