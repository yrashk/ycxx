#!/usr/bin/env python3
"""Process-level daemon tests (daemon design §7, §8.2, #98).

    tests/daemon/process.py <ycxx> <ycxxc_daemon_process>

Covered, each answer compared with a cold (in-process) compile of the same files:
- an edit that lands while a build runs: the build is restarted (§4.1), and its answer is the
  final state's, with the restart counted (§8.1);
- a client killed mid-compile: its request stops (§3.2), the daemon counts it cancelled, and the
  next request is right;
- the daemon killed mid-request: the client starts a new one and asks again (§7.2);
- shutdown, then a new daemon on the next request (§7.4);
- several clients at once, on different files and on the same one, checking that distinct
  build roots actually overlap (#139);
- the trace browser through the daemon (#939): while a build is parked, it already shows the cells
  that ran and the build still running.

Not yet: the cache across a restart (`executed` below a cold run, `durable + validated > 0`,
#17), and every answer at 1 and P threads (#139).

The fixture daemon acknowledges a one-shot checkpoint inside each selected build, after it has
read and analysed its sources. An edit or disconnect wakes the worker through the engine's own
stop token. For the concurrency check, one root stays parked until a distinct root enters.
"""
import os, platform, shutil, signal, socket, subprocess, sys, tempfile, time
from contextlib import ExitStack
from pathlib import Path

failures = []
GATES = {
    'mid.cpp': 'stop',
    'moved.cpp': 'stop',
    'deleted.cpp': 'stop',
    'cancel.cpp': 'stop',
    'c0.cpp': 'release',
    'c1.cpp': 'observe',
    'kill.cpp': 'stop',
    'live.cpp': 'release',
}
# ycxx builds AArch64 Mach-O only (DECISIONS §1.3), so its programs run only on Apple arm64, as
# `run darwin-arm64` in tests/features/ycxx.txt; outputs are compared byte for byte everywhere.
# PARITY: #390: running the built program on Linux needs a hosted link there.
CAN_RUN = sys.platform == 'darwin' and platform.machine() == 'arm64'


def fail(what):
    failures.append(what)
    print(f'FAILED: {what}', file=sys.stderr)


class Env:
    """A private daemon's world: a short runtime directory and a scope marked `.ycxx`."""

    def __init__(self, ycxx, driver=None):
        self.ycxx = ycxx
        self.driver = ycxx if driver is None else driver
        self.runtime = tempfile.mkdtemp(prefix='yp', dir='/tmp')  # sun_path holds 104 bytes on macOS
        os.chmod(self.runtime, 0o700)
        self.scope = Path(tempfile.mkdtemp(prefix='ycxx-process-'))
        (self.scope / '.ycxx').write_text('')
        base = {k: v for k, v in os.environ.items() if k not in ('YCXX_DAEMON', 'YCXX_NO_DAEMON', 'YCXX_ROOT', 'YCXX_NO_WATCH')}
        base['YCXX_RUNTIME_DIR'] = self.runtime
        base['YCXX_DAEMON_IDLE_EXIT'] = '300'
        if driver is not None:
            base['YCXX_THREADS'] = '2'
        base['XDG_CACHE_HOME'] = str(self.scope / '.cache')  # the persistent cache, once #17 has one
        self.base = base
        self.clients = []  # every client started, killed on close
        self.pids = set()  # every daemon seen, killed on close
        self.gates = {}
        self.fixture_daemon = None
        self.daemon = dict(base, YCXX_DAEMON='require')
        self.cold = dict(base, YCXX_NO_DAEMON='1')

    def cxx(self, args, cwd, env, timeout=300):
        exe = self.ycxx if env is self.cold else self.driver
        p = subprocess.run([exe, 'cxx', *args], cwd=cwd, env=env, capture_output=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr

    def start_fixture_daemon(self):
        ready, writer = socket.socketpair()
        with ready, ExitStack() as children:
            children.enter_context(writer)
            fds = [writer.fileno()]
            args = [self.driver, 'serve-process-test', str(fds[0])]
            for source, mode in GATES.items():
                peer, child = socket.socketpair()
                children.enter_context(child)
                self.gates[source] = peer
                fds.append(child.fileno())
                args.extend((mode, source, str(fds[-1])))
            with (Path(self.runtime) / 'fixture.log').open('wb') as log:
                self.fixture_daemon = subprocess.Popen(args, cwd=self.scope, env=self.base, pass_fds=fds,
                                                       stdout=log, stderr=log)
            children.close()
            ready.settimeout(15)
            if ready.recv(1) != b'r':
                raise RuntimeError('the fixture daemon did not signal readiness')

    def expect_gate(self, source, expected):
        peer = self.gates[source]
        peer.settimeout(15)
        try:
            got = peer.recv(1)
        except TimeoutError as e:
            raise RuntimeError(f'{source}: timed out waiting for build checkpoint {expected!r}') from e
        if got != expected:
            raise RuntimeError(f'{source}: expected build checkpoint {expected!r}, got {got!r}')

    def start(self, args, cwd):
        c = subprocess.Popen([self.driver, 'cxx', '-v', *args], cwd=cwd, env=self.daemon, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
        self.clients.append(c)
        return c

    def status(self):
        p = subprocess.run([self.driver, 'ctl', 'status'], cwd=self.scope, env=self.base, capture_output=True, text=True,
                           timeout=60)
        if p.returncode != 0:
            return {}
        st = {k: v for k, v in (line.split(': ', 1) for line in p.stdout.splitlines() if ': ' in line)}
        if int(st.get('pid', 0)) > 1:
            self.pids.add(int(st['pid']))
        return st

    def number(self, field):
        return int(self.status().get(field, -1))

    def pid(self):
        """The daemon's pid; None (and a failure) when no daemon answers `ctl status`."""
        pid = self.number('pid')
        if pid <= 1:
            fail('no daemon answers `ctl status`')
            return None
        return pid

    def same_daemon(self, test, pid):
        """A client restarts a daemon that died and asks again, which would hide a crash: the
        daemon that answered before the test must still be the one answering."""
        now = self.number('pid')
        if now != pid:
            fail(f'{test}: the daemon changed (pid {pid} before, {now} after): it died and a client restarted it')

    def close(self):
        """Every step survives a failure of the one before: clients killed (so none retries into
        a new daemon), the daemon asked to shut down, every daemon seen killed if it lingers,
        the log's tail printed after a failure, and the directories removed last."""
        for peer in self.gates.values():
            peer.close()  # releases parked workers even if this script is unwinding a failed assertion
        for c in self.clients:
            if c.poll() is None:
                c.kill()
                c.wait()
        try:
            self.status()
            subprocess.run([self.driver, 'ctl', 'shutdown'], cwd=self.scope, env=self.base, capture_output=True, timeout=30)
        except subprocess.TimeoutExpired:
            pass
        if self.fixture_daemon is not None:
            try:
                self.fixture_daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.fixture_daemon.kill()
                self.fixture_daemon.wait()
            self.pids.discard(self.fixture_daemon.pid)
        for pid in self.pids:
            gone(pid, 5)
        if failures:
            for log in Path(self.runtime).glob('*.log'):
                print(f'--- the daemon\'s log ({log.name}), last lines:', file=sys.stderr)
                print('\n'.join(log.read_text(errors='replace').splitlines()[-40:]), file=sys.stderr)
        shutil.rmtree(self.scope, ignore_errors=True)
        shutil.rmtree(self.runtime, ignore_errors=True)


def gone(pid, seconds):
    """Waits for `pid` to exit; kills it after `seconds`. Never signals a pid of 1 or less (-1 is
    every process of the user)."""
    if pid is None or pid <= 1:
        return
    for _ in range(int(seconds * 20)):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.05)
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def wait_until(pred, seconds, step=0.02):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(step)
    return False


def slow_source(name, value):
    """A long-running compile fixture shared with daemon.lsp."""
    statements = 40000
    body = '\n'.join('  x = x + 1;' for _ in range(statements))
    return f'int {name}() {{\n  int x = 0;\n{body}\n  return x - {statements} + {value};\n}}\n'


def bytes_of(path):
    return path.read_bytes() if path.exists() else None


def cold_build(env, files, args, out_name):
    """The bytes of `out_name` and the answer of a cold compile of `files`."""
    d = Path(tempfile.mkdtemp(prefix='ycxx-cold-', dir=env.scope.parent))
    try:
        for name, text in files.items():
            (d / name).write_text(text)
        r = env.cxx(args, d, env.cold)
        return r[0], bytes_of(d / out_name)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def restarts_of(stderr):
    """The restarts `-v` reports for a daemon's answer."""
    text = stderr.decode(errors='replace')
    marker = ' restarts'
    i = text.find(marker)
    if i < 0:
        return None
    j = text.rfind('(', 0, i)
    return int(text[j + 1:i]) if j >= 0 and text[j + 1:i].isdigit() else None


def edit_during_build(env):
    """An edit after the root read the old text: it restarts and answers the final state."""
    d = env.scope / 'edit'
    d.mkdir()
    files = {'mid.cpp': 'int mid() { return 2; }\n', 'main.cpp': 'int mid();\nint main() { return mid(); }\n'}
    args = ['--target=arm64-apple-darwin', 'mid.cpp', 'main.cpp', '-o', 'prog']
    for name, text in files.items():
        (d / name).write_text(text)
    files['mid.cpp'] = 'int mid() { return 20; }\n'
    replacement = d / 'replacement'
    replacement.write_text(files['mid.cpp'])
    client = env.start(args, d)
    env.expect_gate('mid.cpp', b'P')
    before = env.status()
    replacement.replace(d / 'mid.cpp')
    env.expect_gate('mid.cpp', b'S')
    _, err = client.communicate(timeout=60)
    want_code, want_bytes = cold_build(env, files, args, 'prog')
    if want_code != 0 or want_bytes is None or client.returncode != want_code or bytes_of(d / 'prog') != want_bytes:
        fail(f'edit during a build: the answer differs from the cold compile of the edit '
             f'(exit {client.returncode}, cold {want_code}, stderr {err[-300:]!r})')
    elif CAN_RUN:
        run = subprocess.run([str(d / 'prog')], capture_output=True, timeout=60)
        if run.returncode != 20:
            fail(f'edit during a build: the program returned {run.returncode}, want 20')
    after = env.status()
    if (restarts_of(err) or 0) < 1:
        fail('edit during a build: the client reported no restart')
    for field in ('restarts', 'revision', 'inputs_changed'):
        if field not in before or field not in after or int(after[field]) <= int(before[field]):
            fail(f'edit during a build: {field} did not advance ({before.get(field)} to {after.get(field)})')


def edit_error_during_build(env):
    """Move an analysed error or delete its file, and compare the final diagnostics with cold."""
    d = env.scope / 'edit-error'
    d.mkdir()
    base = 'int err() { return missing; }\n'
    moved = '// one\n// two\n// three\n// four\nint pad = 0;\nint err() {\n  return missing;\n}\n'
    for name, final in (('moved.cpp', moved), ('deleted.cpp', None)):
        source = d / name
        source.write_text(base)
        replacement = d / 'replacement'
        if final is not None:
            replacement.write_text(final)
        args = ['-c', name]
        client = env.start(args, d)
        env.expect_gate(name, b'P')
        if final is None:
            source.unlink()
        else:
            replacement.replace(source)
        env.expect_gate(name, b'S')
        out, err = client.communicate(timeout=60)
        with tempfile.TemporaryDirectory(prefix='ycxx-cold-', dir=env.scope.parent) as cold:
            if final is not None:
                (Path(cold) / name).write_text(final)
            want_code, want_out, want_err = env.cxx(args, cold, env.cold)
        got_err = b''.join(l + b'\n' for l in err.splitlines() if not l.startswith(b'ycxx: answered by daemon'))
        if want_code != 1 or (client.returncode, out, got_err) != (want_code, want_out, want_err):
            fail(f'edit of {name}: {client.returncode} {got_err!r:.300} != cold {want_code} {want_err!r:.300}')
        if (restarts_of(err) or 0) < 1:
            fail(f'edit of {name}: the client reported no restart')


def killed_client(env):
    """A client killed mid-compile: its request stops and is counted; the next one is right."""
    d = env.scope / 'killed-client'
    d.mkdir()
    files = {'cancel.cpp': 'int main() { return 7; }\n'}
    for name, text in files.items():
        (d / name).write_text(text)
    args = ['--target=arm64-apple-darwin', 'cancel.cpp', '-o', 'prog']
    cancelled = env.number('cancelled')
    client = env.start(args, d)
    env.expect_gate('cancel.cpp', b'P')
    client.kill()
    client.wait(timeout=10)
    env.expect_gate('cancel.cpp', b'S')
    if not wait_until(lambda: env.number('live') == 0, 15):
        fail('killed client: the request is still live')
    if env.number('cancelled') != cancelled + 1:
        fail(f'killed client: `cancelled` went from {cancelled} to {env.number("cancelled")}, want +1')
    if (d / 'prog').exists():
        fail('killed client: the cancelled request wrote an output')
    code, _, _ = env.cxx(args, d, env.daemon)
    want_code, want_bytes = cold_build(env, files, args, 'prog')
    if want_code != 0 or want_bytes is None or code != want_code or bytes_of(d / 'prog') != want_bytes:
        fail(f'killed client: the next request\'s answer differs from a cold compile (exit {code}, cold {want_code})')


def killed_daemon(env):
    """The daemon killed mid-request: the client starts a new one and gets the right answer."""
    d = env.scope / 'killed-daemon'
    d.mkdir()
    files = {'kill.cpp': 'int main() { return 5; }\n'}
    for name, text in files.items():
        (d / name).write_text(text)
    args = ['--target=arm64-apple-darwin', 'kill.cpp', '-o', 'prog']
    client = env.start(args, d)
    env.expect_gate('kill.cpp', b'P')
    pid = env.pid()
    if pid != env.fixture_daemon.pid:
        raise RuntimeError(f'killed daemon: fixture pid {env.fixture_daemon.pid}, status pid {pid}')
    os.kill(pid, signal.SIGKILL)
    env.fixture_daemon.wait(timeout=10)
    _, err = client.communicate(timeout=60)
    want_code, want_bytes = cold_build(env, files, args, 'prog')
    if want_code != 0 or want_bytes is None or client.returncode != want_code or bytes_of(d / 'prog') != want_bytes:
        fail(f'killed daemon: the client\'s answer differs from a cold compile (exit {client.returncode}): {err[-300:]!r}')
    if env.number('pid') in (pid, -1):
        fail('killed daemon: no new daemon answered')


def restart(env):
    """Shutdown, then the next request starts a new daemon and answers right."""
    d = env.scope / 'restart'
    d.mkdir()
    (d / 'r.cpp').write_text('int main() { return 6; }\n')
    args = ['-c', 'r.cpp', '-o', 'r.o']
    env.cxx(args, d, env.daemon)
    pid = env.pid()
    if pid is None:
        return
    stop = subprocess.run([env.driver, 'ctl', 'shutdown'], cwd=env.scope, env=env.base, capture_output=True, timeout=15)
    if stop.returncode != 0 or not wait_until(lambda: not env.status(), 15):
        fail(f'restart: daemon {pid} did not shut down: {stop.stderr!r}')
        return
    (d / 'r.o').unlink(missing_ok=True)
    code, _, _ = env.cxx(args, d, env.daemon)
    want_code, want_bytes = cold_build(env, {'r.cpp': (d / 'r.cpp').read_text()}, args, 'r.o')
    if want_code != 0 or want_bytes is None or code != want_code or bytes_of(d / 'r.o') != want_bytes:
        fail('restart: the new daemon\'s answer differs from a cold compile')
    if env.number('pid') in (pid, -1):
        fail('restart: the same daemon answered after shutdown')
    # Without the persistent cache (#17) the new daemon executes as much as a cold run; with it,
    # `executed` must drop below that and `durable + validated` be positive (§8.2).


def concurrent_clients(env):
    """Four files, one requested twice, and two distinct roots in the engine together (#139)."""
    d = env.scope / 'concurrent'
    d.mkdir()
    sources = {f'c{i}.cpp': f'int f{i}() {{ return {i}; }}\n' for i in range(4)}
    for name, text in sources.items():
        (d / name).write_text(text)
    jobs = [('c0.cpp', 'c0-0.o'), ('c0.cpp', 'c0-1.o'), ('c1.cpp', 'c1.o'), ('c2.cpp', 'c2.o'), ('c3.cpp', 'c3.o')]
    requests = env.number('requests')
    first, same, *distinct = [['-c', src, '-o', obj] for src, obj in jobs]
    clients = [env.start(first, d)]
    env.expect_gate('c0.cpp', b'P')
    clients.append(env.start(same, d))
    if not wait_until(lambda: env.number('peak_running') >= 2, 10):
        raise RuntimeError('concurrent clients: the same-file request did not enter the session')
    clients.extend(env.start(args, d) for args in distinct)
    env.expect_gate('c1.cpp', b'P')
    env.gates['c0.cpp'].sendall(b'R')
    env.expect_gate('c0.cpp', b'R')
    for (src, obj), client in zip(jobs, clients):
        _, err = client.communicate(timeout=60)
        want_code, want_bytes = cold_build(env, {src: sources[src]}, ['-c', src, '-o', obj], obj)
        if want_code != 0 or want_bytes is None or client.returncode != want_code or bytes_of(d / obj) != want_bytes:
            fail(f'concurrent clients: {obj} differs from a cold compile: {err[-300:]!r}')
    if env.number('requests') != requests + len(jobs):
        fail(f'concurrent clients: {len(jobs)} requests sent, {env.number("requests") - requests} answered')


def live_trace(env):
    """`--trace-engine=interactive` through the daemon, under a pty (#939): parked at its checkpoint,
    after it has read and analysed its sources, the build has not ended, and the browser already
    shows what ran and Build running; released, it ends, the browser says so, and `q` leaves with
    the compile's status and output."""
    import trace_tui
    d = env.scope / 'live'
    d.mkdir()
    source = 'int square(int x) { return x * x; }\nint main() { return square(3) - 9; }\n'
    (d / 'live.cpp').write_text(source)
    tui_env = {k: v for k, v in env.daemon.items() if k != 'NO_COLOR'}
    tui_env.update(TERM='xterm-256color', YCXX_CLIPBOARD='osc52')
    t = trace_tui.Tui(env.driver, d, tui_env, 'live.cpp')
    released = False
    try:
        env.expect_gate('live.cpp', b'P')

        def text(o):
            return '\n'.join(trace_tui.plain_lines(trace_tui.Tui.frame_of(o)))

        def parked(o):
            f = text(o)
            return '● live' in f and 'Build(' in f and 'running' in f and 'TuFront(' in f and 'executed' in f
        if not t.read_until(parked, timeout=20):
            fail(f'live trace: while the build is parked, the browser shows no cells: {text(t.out)[:600]!r}')
        env.gates['live.cpp'].sendall(b'R')
        env.expect_gate('live.cpp', b'R')
        released = True
        if not t.read_until(lambda o: 'the build has ended' in text(o), timeout=60):
            fail(f'live trace: the browser never says the build has ended: {text(t.out)[:600]!r}')
        elif ' running' in text(t.out).split('\n', 1)[0]:
            fail('live trace: cells still running after the build ended')
        t.send(b'q')
        status, cooked = t.wait(timeout=60)
        want_code, want_bytes = cold_build(env, {'live.cpp': source}, trace_tui.COMPILE + ['live.cpp', '-o', 'a.o'], 'a.o')
        if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != want_code or bytes_of(d / 'a.o') != want_bytes or not cooked:
            fail(f'live trace: the browsed build differs from a cold compile (status {status}, cold {want_code})')
    finally:
        if not released:
            env.gates['live.cpp'].sendall(b'R')
        if t.leader:
            try:
                os.kill(t.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


def main():
    ycxx, driver = map(os.path.abspath, sys.argv[1:])
    env = Env(ycxx, driver)
    try:
        env.start_fixture_daemon()
        # A first request checks the fixture daemon. Every scenario that does not replace it
        # must leave the same daemon answering.
        (env.scope / 'first.cpp').write_text('int main() { return 0; }\n')
        if (env.cxx(['-c', 'first.cpp', '-o', 'first.o'], env.scope, env.daemon)[0] != 0 or
                env.pid() != env.fixture_daemon.pid):
            fail('the first request through the daemon failed: no test can run')
            return 1
        for test in (edit_during_build, edit_error_during_build, killed_client, concurrent_clients, live_trace, killed_daemon, restart):
            t0 = time.monotonic()
            pid = env.pid()
            if pid is None:
                break
            test(env)
            if test not in (killed_daemon, restart):
                env.same_daemon(test.__name__, pid)
            if env.number('live') != 0:
                fail(f'{test.__name__}: daemon still reports live requests')
            print(f'{test.__name__}: {time.monotonic() - t0:.1f}s')
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as e:
        fail(str(e))
    finally:
        env.close()
    if failures:
        print(f'daemon.process: {len(failures)} failure(s)', file=sys.stderr)
        return 1
    print('daemon.process: an edit mid-build, a killed client, a killed daemon, a restart and concurrent clients: as cold')
    return 0


if __name__ == '__main__':
    sys.exit(main())
