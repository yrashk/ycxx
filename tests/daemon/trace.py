#!/usr/bin/env python3
"""The engine trace through the daemon (#245).

    tests/daemon/trace.py <ycxx>

- `ycxx cxx --trace-engine` through a private daemon: a cold build executes cells, a second
  build of the same files executes none and shows its root reused, a whitespace edit shows early
  cutoff; the output equals an untraced in-process build's, and a non-terminal gets no colors.
- `ycxx ctl trace` attached to the daemon sees each request under its own header, and exits when
  the daemon shuts down.
- A watcher that stops reading never slows builds, and does not keep the daemon from stopping
  (LESSONS: a long-lived connection's lifetime).
- `ycxx cxx --watch --trace-engine` builds again when a header the build read changes, and only
  then: an unrelated file changes nothing.
"""
import os, subprocess, sys, tempfile, threading, time, shutil
from pathlib import Path

failures = []


def fail(what):
    failures.append(what)
    print(f'FAILED: {what}', file=sys.stderr)


def check(ok, what):
    if not ok:
        fail(what)


SOURCE = 'int square(int x) { return x * x; }\nint main() { return square(3) - 9; }\n'
EDITED = 'int square(int x) {\n  return x  *  x;\n}\nint main() { return square(3) - 9; }\n'


def main(ycxx):
    runtime = tempfile.mkdtemp(prefix='yt', dir='/tmp')  # sun_path holds 104 bytes on macOS
    os.chmod(runtime, 0o700)
    scope = Path(tempfile.mkdtemp(prefix='ycxx-trace-'))
    (scope / '.ycxx').write_text('')
    base = {k: v for k, v in os.environ.items() if k not in ('YCXX_DAEMON', 'YCXX_NO_DAEMON', 'YCXX_ROOT', 'NO_COLOR')}
    base.update(YCXX_RUNTIME_DIR=runtime, YCXX_DAEMON_IDLE_EXIT='300', YCXX_THREADS='4')
    daemon = dict(base, YCXX_DAEMON='require')
    watchers = []
    try:
        src = scope / 'a.cpp'
        src.write_text(SOURCE)

        def cxx(*args, env=daemon):
            return subprocess.run([ycxx, 'cxx', *args], cwd=scope, env=env, capture_output=True, text=True, timeout=300)

        def ctl(*args):
            return subprocess.run([ycxx, 'ctl', *args], cwd=scope, env=base, capture_output=True, text=True, timeout=60)

        def lines(stderr):
            return [l for l in stderr.splitlines() if l[:2] in ('* ', '~ ', '+ ', '= ', '# ', '. ', '! ') or l.startswith('  ')]

        # The output's name is in its code signature: compare builds of one name.
        cold = cxx('a.cpp', '-o', 'a', env=dict(base, YCXX_NO_DAEMON='1'))
        check(cold.returncode == 0, f'an untraced in-process build: {cold.stderr}')
        untraced = (scope / 'a').read_bytes()

        # `ctl trace` never starts a daemon: one is started by a build of something else first.
        (scope / 'other.cpp').write_text('int main() { return 0; }\n')
        check(cxx('other.cpp', '-o', 'other').returncode == 0, 'a build starts the daemon')
        w = subprocess.Popen([ycxx, 'ctl', 'trace'], cwd=scope, env=base, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True)
        watchers.append(w)
        hello = w.stdout.readline()
        check(hello.startswith('ycxx: watching'), f'the watcher attaches: {hello}')
        seen = []  # what it prints, drained as it comes, so it never stalls
        drain = threading.Thread(target=lambda: seen.append(w.stdout.read()), daemon=True)
        drain.start()
        first = cxx('--trace-engine', 'a.cpp', '-o', 'a')
        check(first.returncode == 0, f'the cold traced build: {first.stderr}')
        check((scope / 'a').read_bytes() == untraced, 'a traced build writes what an untraced one does')
        check('\x1b[' not in first.stderr, 'no colors on a non-terminal')
        check(any(' executed' in l for l in lines(first.stderr)), 'a cold build executes cells')
        check('Build(a.cpp → a)' in first.stderr and 'Lower(square)' in first.stderr, 'keys read as files and functions')
        check(first.stderr.rstrip().splitlines()[-1].startswith('trace: '), 'the summary line ends the stream')

        second = cxx('--trace-engine', 'a.cpp', '-o', 'a')
        check(second.returncode == 0, 'the second traced build')
        traced = lines(second.stderr)
        check(not any(' executed' in l for l in traced), f'a second build executes nothing: {traced}')
        check(any(l.startswith('= Build(a.cpp → a) reused') for l in traced), f'a second build reuses its root: {traced}')

        src.write_text(EDITED)
        third = cxx('--trace-engine', 'a.cpp', '-o', 'a')
        check(third.returncode == 0, 'the edited traced build')
        check(any(' cutoff' in l for l in lines(third.stderr)), 'a whitespace edit shows early cutoff')
        check(not any('Lower(square) executed' in l for l in lines(third.stderr)), 'a whitespace edit does not lower again')

        bad = cxx('--trace-engine=sideways', 'a.cpp')
        check(bad.returncode == 1 and 'unknown trace mode' in bad.stderr, 'an unknown mode is an error')

        # --watch: a build, then one per change of a file the build read.
        (scope / 'k.h').write_text('inline int k() { return 1; }\n')
        src.write_text('#include "k.h"\n' + SOURCE.replace('- 9', '- 8 - k()'))
        wp = subprocess.Popen([ycxx, 'cxx', '--watch', '--trace-engine', 'a.cpp', '-o', 'w'], cwd=scope, env=daemon,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        watchers.append(wp)
        log = []
        threading.Thread(target=lambda: log.extend(iter(wp.stderr.readline, '')), daemon=True).start()

        def wait_for(what, times=1, timeout=60):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if sum(what in l for l in list(log)) >= times:
                    return True
                time.sleep(0.1)
            return False

        check(wait_for('watching the files it read'), f'--watch builds once and waits: {"".join(log)[-400:]}')
        (scope / 'unrelated.txt').write_text('x')
        time.sleep(2)
        check(not any('── build 2' in l for l in log), 'a file no build read starts no build')
        (scope / 'k.h').write_text('inline int k() { return 0; }\n')
        check(wait_for('── build 2'), f'a header the build read starts a build: {"".join(log)[-400:]}')
        check(wait_for('trace: ', times=2), 'the rebuild is traced')
        second = ''.join(log[next(i for i, l in enumerate(log) if '── build 2' in l):])
        check('Lower(k) executed' in second or 'FunctionIr(k) executed' in second, f'k is built again: {second[:600]}')
        # An edit that another client's build applies first still starts a build here.
        check(wait_for('watching the files it read', times=2), 'the rebuild waits again')
        (scope / 'k.h').write_text('inline int k() { return 1; }\n')
        other = cxx('a.cpp', '-o', 'other')
        check(other.returncode == 0, 'another client builds meanwhile')
        check(wait_for('── build 3'), 'an edit another request applied is not lost')
        wp.kill()
        wp.wait(timeout=20)

        # A stalled watcher: `ctl trace` writing into a pipe nobody reads, so it stops reading too.
        stalled = subprocess.Popen([ycxx, 'ctl', 'trace'], cwd=scope, env=base, stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL)
        watchers.append(stalled)
        time.sleep(0.5)
        t0 = time.monotonic()
        for i in range(40):
            src.write_text(SOURCE.replace('9', str(9 + i)))  # every build executes something
            r = cxx('a.cpp', '-o', 'a')
            check(r.returncode == 0, f'build {i} beside a stalled watcher')
        check(time.monotonic() - t0 < 120, 'a stalled watcher does not slow builds down')

        r = ctl('shutdown')
        check(r.returncode == 0, f'shutdown: {r.stderr}')
        try:
            w.wait(timeout=20)
        except subprocess.TimeoutExpired:
            fail('the reading watcher outlived the daemon')
        # The stalled one is blocked on its own stdout; the daemon must have stopped anyway.
        deadline = time.monotonic() + 20
        while ctl('status').returncode == 0 and time.monotonic() < deadline:
            time.sleep(0.2)
        check(ctl('status').returncode != 0, 'a stalled watcher does not keep the daemon alive')
        drain.join(timeout=20)
        out = ''.join(seen)
        check('── #' in out and 'cxx a.cpp -o a' in out, f'the watcher shows each request: {out[:400]}')
        check('Build(a.cpp → a)' in out and 'trace: ' in out, 'the watcher shows the events and each summary')
        check('the daemon has gone' in out, 'the watcher says the daemon went')
    finally:
        for p in watchers:
            if p.poll() is None:
                p.kill()
        subprocess.run([ycxx, 'ctl', 'shutdown'], cwd=scope, env=base, capture_output=True, timeout=60)
        shutil.rmtree(scope, ignore_errors=True)
        shutil.rmtree(runtime, ignore_errors=True)
    if failures:
        print(f'daemon.trace: {len(failures)} failed', file=sys.stderr)
        return 1
    print('daemon.trace: all passed')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
