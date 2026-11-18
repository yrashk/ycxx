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

A slow translation unit (tens of thousands of statements, a few seconds) opens the window in
which an edit or a kill lands. Each variant changes its constant, so the daemon cannot answer it
from cells it already holds.
"""
import os, signal, subprocess, sys, tempfile, threading, time, shutil
from pathlib import Path

failures = []
SLOW_STATEMENTS = 40000


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
        self.base = base
        self.daemon = dict(base, YCXX_DAEMON='require')
        self.cold = dict(base, YCXX_NO_DAEMON='1')

    def cxx(self, args, cwd, env, timeout=300):
        p = subprocess.run([self.ycxx, 'cxx', *args], cwd=cwd, env=env, capture_output=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr

    def start(self, args, cwd):
        return subprocess.Popen([self.ycxx, 'cxx', '-v', *args], cwd=cwd, env=self.daemon, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)

    def status(self):
        p = subprocess.run([self.ycxx, 'ctl', 'status'], cwd=self.scope, env=self.base, capture_output=True, text=True,
                           timeout=60)
        if p.returncode != 0:
            return {}
        return {k: v for k, v in (line.split(': ', 1) for line in p.stdout.splitlines() if ': ' in line)}

    def number(self, field):
        return int(self.status().get(field, -1))

    def same_daemon(self, test, pid):
        """A client restarts a daemon that died and asks again, which would hide a crash: the
        daemon that answered before the test must still be the one answering."""
        now = self.number('pid')
        if now != pid:
            fail(f'{test}: the daemon changed (pid {pid} before, {now} after): it died and a client restarted it')

    def close(self):
        pid = self.status().get('pid')
        subprocess.run([self.ycxx, 'ctl', 'shutdown'], cwd=self.scope, env=self.base, capture_output=True, timeout=60)
        if pid:
            gone(int(pid), 5)
        shutil.rmtree(self.scope, ignore_errors=True)
        shutil.rmtree(self.runtime, ignore_errors=True)


def gone(pid, seconds):
    """Waits for `pid` to exit; kills it after `seconds`."""
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
    args = ['slow.cpp', 'mid.cpp', 'main.cpp', '-o', 'prog']
    for attempt in range(3):
        files['slow.cpp'] = slow_source('slow', attempt + 1)
        files['mid.cpp'] = 'int mid() { return 2; }\n'
        for name, text in files.items():
            (d / name).write_text(text)
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
        run = subprocess.run([str(d / 'prog')], capture_output=True, timeout=60)
        if run.returncode != attempt + 1 + 20 + attempt:
            fail(f'edit during a build: the program returned {run.returncode}, want {attempt + 1 + 20 + attempt}')
        if env.number('revision') <= revision_before or env.number('inputs_changed') <= changed_before:
            fail('edit during a build: `ctl status` counts no new revision or changed input (§8.1)')
        if (restarts_of(err) or 0) >= 1:
            if env.number('restarts') < restarts_before + 1:
                fail('edit during a build: `ctl status` does not count the restart')
            return
    fail('an edit during a build never restarted it (3 attempts)')


def killed_client(env):
    """A client killed mid-compile: its request stops and is counted; the next one is right."""
    d = env.scope / 'killed-client'
    d.mkdir()
    files = {'a.cpp': slow_source('a', 3), 'b.cpp': slow_source('b', 4), 'main.cpp': 'int a(); int b();\nint main() { return a() + b(); }\n'}
    for name, text in files.items():
        (d / name).write_text(text)
    args = ['a.cpp', 'b.cpp', 'main.cpp', '-o', 'prog']
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
    args = ['k.cpp', 'main.cpp', '-o', 'prog']
    client = env.start(args, d)
    if not wait_until(lambda: env.number('live') >= 1, 30):
        fail('killed daemon: the request never showed as live')
    pid = env.number('pid')
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
    pid = env.number('pid')
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
    """Six clients at once: four files, and one file twice. Each answer equals a cold compile."""
    d = env.scope / 'concurrent'
    d.mkdir()
    sources = {f'c{i}.cpp': f'int f{i}() {{ return {i}; }}\nint main() {{ return f{i}() * 2; }}\n' for i in range(4)}
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


def main():
    ycxx = os.path.abspath(sys.argv[1])
    env = Env(ycxx)
    try:
        # A first request starts the daemon; every test but the two that replace it on purpose
        # must leave the same daemon answering.
        (env.scope / 'first.cpp').write_text('int main() { return 0; }\n')
        env.cxx(['-c', 'first.cpp', '-o', 'first.o'], env.scope, env.daemon)
        for test in (edit_during_build, killed_client, killed_daemon, restart, concurrent_clients):
            t0 = time.monotonic()
            pid = env.number('pid')
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
