#!/usr/bin/env python3
"""Process-level daemon tests (daemon design §7, §8.2, #98).

    tests/daemon/process.py <ycxx>

Covered, each answer compared with a cold (in-process) compile of the same files:
- an edit that lands while a build runs: the build is restarted (§4.1), and its answer is the
  final state's, with the restart counted (§8.1);
- a client killed mid-compile: its request stops (§3.2), the daemon counts it cancelled, and the
  next request is right;
- the daemon killed mid-request: the client starts a new one and asks again (§7.2);
- shutdown, then a new daemon on the next request (§7.4);
- several clients at once, on different files and on the same one (they queue today; #139 makes
  them concurrent, and this test then checks them running at once).

Not yet: the cache across a restart (`executed` below a cold run, `durable + validated > 0`,
#17), and every answer at 1 and P threads (#139).

A slow translation unit (tens of thousands of statements: 1.5-3 s in the Debug build CI uses;
a Release build would need more) opens the window in which an edit or a kill lands. Each variant
changes its constant, so the daemon cannot answer it from cells it already holds.
"""
import os, platform, signal, subprocess, sys, tempfile, threading, time, shutil
from pathlib import Path

failures = []
SLOW_STATEMENTS = 40000
# ycxx builds AArch64 Mach-O only (DECISIONS §1.3), so its programs run only on Apple arm64, as
# `run darwin-arm64` in tests/features/ycxx.txt; outputs are compared byte for byte everywhere.
# PARITY: #390: running the built program on Linux needs a hosted link there.
CAN_RUN = sys.platform == 'darwin' and platform.machine() == 'arm64'


def fail(what):
    failures.append(what)
    print(f'FAILED: {what}', file=sys.stderr)


class Env:
    """A private daemon's world: a short runtime directory and a scope marked `.ycxx`."""

    def __init__(self, ycxx):
        self.ycxx = ycxx
        self.runtime = tempfile.mkdtemp(prefix='yp', dir='/tmp')  # sun_path holds 104 bytes on macOS
        os.chmod(self.runtime, 0o700)
        self.scope = Path(tempfile.mkdtemp(prefix='ycxx-process-'))
        (self.scope / '.ycxx').write_text('')
        base = {k: v for k, v in os.environ.items() if k not in ('YCXX_DAEMON', 'YCXX_NO_DAEMON', 'YCXX_ROOT', 'YCXX_NO_WATCH')}
        base['YCXX_RUNTIME_DIR'] = self.runtime
        base['YCXX_DAEMON_IDLE_EXIT'] = '300'
        base['XDG_CACHE_HOME'] = str(self.scope / '.cache')  # the persistent cache, once #17 has one
        self.base = base
        self.clients = []  # every client started, killed on close
        self.pids = set()  # every daemon seen, killed on close
        self.daemon = dict(base, YCXX_DAEMON='require')
        self.cold = dict(base, YCXX_NO_DAEMON='1')

    def cxx(self, args, cwd, env, timeout=300):
        p = subprocess.run([self.ycxx, 'cxx', *args], cwd=cwd, env=env, capture_output=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr

    def start(self, args, cwd):
        c = subprocess.Popen([self.ycxx, 'cxx', '-v', *args], cwd=cwd, env=self.daemon, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
        self.clients.append(c)
        return c

    def status(self):
        p = subprocess.run([self.ycxx, 'ctl', 'status'], cwd=self.scope, env=self.base, capture_output=True, text=True,
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
        for c in self.clients:
            if c.poll() is None:
                c.kill()
                c.wait()
        try:
            self.status()
            subprocess.run([self.ycxx, 'ctl', 'shutdown'], cwd=self.scope, env=self.base, capture_output=True, timeout=30)
        except subprocess.TimeoutExpired:
            pass
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
    body = '\n'.join('  x = x + 1;' for _ in range(SLOW_STATEMENTS))
    return f'int {name}() {{\n  int x = 0;\n{body}\n  return x - {SLOW_STATEMENTS} + {value};\n}}\n'


def cold_build(env, files, args, out_name):
    """The bytes of `out_name` and the answer of a cold compile of `files`."""
    d = Path(tempfile.mkdtemp(prefix='ycxx-cold-', dir=env.scope.parent))
    try:
        for name, text in files.items():
            (d / name).write_text(text)
        r = env.cxx(args, d, env.cold)
        out = d / out_name
        return r[0], out.read_bytes() if out.exists() else None
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
    """An edit while the slow unit compiles: the request restarts and answers the final state."""
    d = env.scope / 'edit'
    d.mkdir()
    files = {'mid.cpp': 'int mid() { return 2; }\n', 'main.cpp': 'int slow(); int mid();\nint main() { return slow() + mid(); }\n'}
    args = ['--target=arm64-apple-darwin', 'slow.cpp', 'mid.cpp', 'main.cpp', '-o', 'prog']
    for attempt in range(3):
        files['slow.cpp'] = slow_source('slow', attempt + 1)
        files['mid.cpp'] = 'int mid() { return 2; }\n'
        for name, text in files.items():
            (d / name).write_text(text)
        # Cheap requests that read every file first: they apply the setup's own batch and give
        # mid.cpp a table entry, so the counters below move only for the edit made mid-build.
        for name in files:
            env.cxx(['-E', name], d, env.daemon)
        restarts_before = env.number('restarts')
        revision_before, changed_before = env.number('revision'), env.number('inputs_changed')
        client = env.start(args, d)
        if not wait_until(lambda: env.number('live') >= 1, 30):
            fail('edit during a build: the request never showed as live')
        time.sleep(0.3)
        files['mid.cpp'] = f'int mid() {{ return {20 + attempt}; }}\n'
        (d / 'mid.cpp').write_text(files['mid.cpp'])
        out, err = client.communicate(timeout=300)
        want_code, want_bytes = cold_build(env, files, args, 'prog')
        got_bytes = (d / 'prog').read_bytes() if (d / 'prog').exists() else None
        if client.returncode != want_code or got_bytes != want_bytes:
            fail(f'edit during a build (attempt {attempt}): the answer is not the final state\'s (exit {client.returncode}, '
                 f'cold {want_code}, outputs {"equal" if got_bytes == want_bytes else "differ"})')
            return
        if CAN_RUN:
            run = subprocess.run([str(d / 'prog')], capture_output=True, timeout=60)
            if run.returncode != attempt + 1 + 20 + attempt:
                fail(f'edit during a build: the program returned {run.returncode}, want {attempt + 1 + 20 + attempt}')
        # An edit to a file the build has not read yet changes no table entry (§5.3), and the build
        # reads the new text anyway; the daemon still restarts it today (#161). Only a restart
        # with a new revision is the one this test is for: an edit to a file already read.
        if (restarts_of(err) or 0) >= 1:
            if env.number('restarts') < restarts_before + 1:
                fail('edit during a build: `ctl status` does not count the restart')
            if env.number('revision') > revision_before and env.number('inputs_changed') > changed_before:
                return
            print(f'edit during a build (attempt {attempt}): restarted with no changed entry (#161); trying again')
    fail('an edit during a build never restarted it (3 attempts)')


def edit_error_during_build(env):
    """An edit that moves an error, and one that deletes a file, while the slow unit compiles:
    the diagnostics and the exit status are the final state's, rendered against the text the
    build compiled (#139)."""
    d = env.scope / 'edit-error'
    d.mkdir()
    base = 'int err() { return missing; }\n'
    moved = '// one\n// two\n// three\n// four\nint pad = 0;\nint err() {\n  return missing;\n}\n'
    for attempt, final in enumerate((moved, None)):
        files = {'slow.cpp': slow_source('slow', attempt + 1), 'err.cpp': base}
        for name, text in files.items():
            (d / name).write_text(text)
        for name in files:
            env.cxx(['-E', name], d, env.daemon)
        args = ['-c', 'slow.cpp', 'err.cpp']
        client = env.start(args, d)
        if not wait_until(lambda: env.number('live') >= 1, 30):
            fail('edit of an error during a build: the request never showed as live')
        time.sleep(0.3)
        if final is None:
            (d / 'err.cpp').unlink()
            del files['err.cpp']
        else:
            (d / 'err.cpp').write_text(final)
            files['err.cpp'] = final
        out, err = client.communicate(timeout=300)
        cold = Path(tempfile.mkdtemp(prefix='ycxx-cold-', dir=env.scope.parent))
        try:
            for name, text in files.items():
                (cold / name).write_text(text)
            want = env.cxx(args, cold, env.cold)
        finally:
            shutil.rmtree(cold, ignore_errors=True)
        got_err = b''.join(l + b'\n' for l in err.splitlines() if not l.startswith(b'ycxx: answered by daemon'))
        if client.returncode != want[0] or got_err != want[2]:
            fail(f'edit of an error during a build ({"deleted" if final is None else "moved"}): '
                 f'{client.returncode} {got_err!r:.300} != cold {want[0]} {want[2]!r:.300}')


def killed_client(env):
    """A client killed mid-compile: its request stops and is counted; the next one is right."""
    d = env.scope / 'killed-client'
    d.mkdir()
    files = {'a.cpp': slow_source('a', 3), 'b.cpp': slow_source('b', 4), 'main.cpp': 'int a(); int b();\nint main() { return a() + b(); }\n'}
    for name, text in files.items():
        (d / name).write_text(text)
    args = ['--target=arm64-apple-darwin', 'a.cpp', 'b.cpp', 'main.cpp', '-o', 'prog']
    cancelled = env.number('cancelled')
    client = env.start(args, d)
    if not wait_until(lambda: env.number('live') >= 1, 30):
        fail('killed client: the request never showed as live')
    time.sleep(0.3)
    client.kill()
    client.wait()
    if not wait_until(lambda: env.number('live') == 0, 120, 0.1):
        fail('killed client: the request is still live')
    if env.number('cancelled') != cancelled + 1:
        fail(f'killed client: `cancelled` went from {cancelled} to {env.number("cancelled")}, want +1')
    code, out, err = env.cxx(args, d, env.daemon)
    want_code, want_bytes = cold_build(env, files, args, 'prog')
    if code != want_code or (d / 'prog').read_bytes() != want_bytes:
        fail(f'killed client: the next request\'s answer differs from a cold compile (exit {code}, cold {want_code})')


def killed_daemon(env):
    """The daemon killed mid-request: the client starts a new one and gets the right answer."""
    d = env.scope / 'killed-daemon'
    d.mkdir()
    files = {'k.cpp': slow_source('k', 5), 'main.cpp': 'int k();\nint main() { return k(); }\n'}
    for name, text in files.items():
        (d / name).write_text(text)
    args = ['--target=arm64-apple-darwin', 'k.cpp', 'main.cpp', '-o', 'prog']
    client = env.start(args, d)
    if not wait_until(lambda: env.number('live') >= 1, 30):
        fail('killed daemon: the request never showed as live')
    pid = env.pid()
    if pid is None:
        client.kill()
        client.wait()
        return
    os.kill(pid, signal.SIGKILL)
    out, err = client.communicate(timeout=300)
    want_code, want_bytes = cold_build(env, files, args, 'prog')
    got = (d / 'prog').read_bytes() if (d / 'prog').exists() else None
    if client.returncode != want_code or got != want_bytes:
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
    subprocess.run([env.ycxx, 'ctl', 'shutdown'], cwd=env.scope, env=env.base, capture_output=True, timeout=60)
    gone(pid, 10)
    code, _, _ = env.cxx(args, d, env.daemon)
    want_code, want_bytes = cold_build(env, {'r.cpp': (d / 'r.cpp').read_text()}, args, 'r.o')
    if code != want_code or (d / 'r.o').read_bytes() != want_bytes:
        fail('restart: the new daemon\'s answer differs from a cold compile')
    if env.number('pid') in (pid, -1):
        fail('restart: the same daemon answered after shutdown')
    # Without the persistent cache (#17) the new daemon executes as much as a cold run; with it,
    # `executed` must drop below that and `durable + validated` be positive (§8.2).


def concurrent_clients(env):
    """Six clients at once: four files, and one file twice. Each answer equals a cold compile, and
    the requests ran in the engine at the same time (#139)."""
    d = env.scope / 'concurrent'
    d.mkdir()
    # Long enough that six requests sent together overlap.
    body = ''.join(f'int g{{i}}_{k}(int x) {{ return x * {k} + {k % 7}; }}\n' for k in range(400))
    sources = {f'c{i}.cpp': body.replace('{i}', str(i)) + f'int f{i}() {{ return {i}; }}\nint main() {{ return f{i}() * 2; }}\n'
               for i in range(4)}
    for name, text in sources.items():
        (d / name).write_text(text)
    jobs = [(f'c{i}.cpp', f'c{i}-{n}.o') for i in range(4) for n in range(1 if i else 2)]
    requests = env.number('requests')
    results = {}

    def one(src, obj):
        results[obj] = env.cxx(['-c', src, '-o', obj], d, env.daemon)

    threads = [threading.Thread(target=one, args=job) for job in jobs]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for src, obj in jobs:
        want_code, want_bytes = cold_build(env, {src: sources[src]}, ['-c', src, '-o', obj], obj)
        if results[obj][0] != want_code or (d / obj).read_bytes() != want_bytes:
            fail(f'concurrent clients: {obj} differs from a cold compile')
    if env.number('requests') != requests + len(jobs):
        fail(f'concurrent clients: {len(jobs)} requests sent, {env.number("requests") - requests} answered')
    if env.number('peak_running') < 2:
        fail(f'concurrent clients: the requests ran one at a time (peak_running {env.number("peak_running")})')


def main():
    ycxx = os.path.abspath(sys.argv[1])
    env = Env(ycxx)
    try:
        # A first request starts the daemon; every test but the two that replace it on purpose
        # must leave the same daemon answering.
        (env.scope / 'first.cpp').write_text('int main() { return 0; }\n')
        if env.cxx(['-c', 'first.cpp', '-o', 'first.o'], env.scope, env.daemon)[0] != 0 or env.pid() is None:
            fail('the first request through the daemon failed: no test can run')
            return 1
        for test in (edit_during_build, edit_error_during_build, killed_client, killed_daemon, restart, concurrent_clients):
            t0 = time.monotonic()
            pid = env.pid()
            if pid is None:
                break
            test(env)
            if test not in (killed_daemon, restart):
                env.same_daemon(test.__name__, pid)
            print(f'{test.__name__}: {time.monotonic() - t0:.1f}s')
    finally:
        env.close()
    if failures:
        print(f'daemon.process: {len(failures)} failure(s)', file=sys.stderr)
        return 1
    print('daemon.process: an edit mid-build, a killed client, a killed daemon, a restart and concurrent clients: as cold')
    return 0


if __name__ == '__main__':
    sys.exit(main())
