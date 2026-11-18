#!/usr/bin/env python3
"""Daemon versus in-process (daemon design §8.2): `ycxx cxx` through a private daemon must give
the same stdout, stderr, exit status and output bytes as under YCXX_NO_DAEMON=1.

    tests/daemon/compare.py <ycxx> <tests/own>

Covered:
- every test of tests/own/slice and tests/own/smoke, linked, with -c, -fsyntax-only and -E;
- the same argument vector from two working directories with different local headers;
- an edit between two requests (the watcher), with the answer equal to a cold compile;
- fallback: an unusable runtime directory runs in-process with the same output;
- a daemon killed between requests leaves a stale socket; the next client starts a new one.

The daemon runs on a private, short YCXX_RUNTIME_DIR and is shut down at the end. #98 owns the
rest of the β daemon tests (tests/daemon: edits during a build, a killed client, a killed daemon
mid-request, the cache across restarts; .incr through the daemon).
"""
import os, shutil, signal, subprocess, sys, tempfile, time
from pathlib import Path

failures = []


def fail(what):
    failures.append(what)
    print(f'FAILED: {what}', file=sys.stderr)


def run(ycxx, args, cwd, env):
    p = subprocess.run([ycxx, 'cxx', *args], cwd=cwd, env=env, capture_output=True, timeout=300)
    return p.returncode, p.stdout, p.stderr


def outputs(d):
    return {f.name: f.read_bytes() for f in sorted(Path(d).iterdir()) if f.is_file()}


def compare(ycxx, name, args, setup, envs):
    """Runs `args` in two fresh copies of a directory prepared by `setup`, in-process and through
    the daemon, and compares everything they produce."""
    results = []
    for mode, env in envs:
        d = Path(tempfile.mkdtemp(prefix='ycxx-cmp-'))
        try:
            setup(d)
            before = set(outputs(d))
            code, out, err = run(ycxx, args, d, env)
            results.append((code, out, err, {k: v for k, v in outputs(d).items() if k not in before}))
        finally:
            shutil.rmtree(d, ignore_errors=True)
    (c0, o0, e0, f0), (c1, o1, e1, f1) = results
    for what, a, b in (('exit status', c0, c1), ('stdout', o0, o1), ('stderr', e0, e1), ('outputs', f0, f1)):
        if a != b:
            fail(f'{name}: {what} differs between in-process and the daemon:\n  in-process: {a!r:.300}\n  daemon:     {b!r:.300}')


def main():
    ycxx, own = os.path.abspath(sys.argv[1]), Path(sys.argv[2]).resolve()
    runtime = tempfile.mkdtemp(prefix='yd', dir='/tmp')  # short: sun_path holds 104 bytes on macOS
    os.chmod(runtime, 0o700)
    base = dict(os.environ)
    for k in ('YCXX_DAEMON', 'YCXX_NO_DAEMON', 'YCXX_ROOT', 'YCXX_NO_WATCH'):
        base.pop(k, None)
    base['YCXX_RUNTIME_DIR'] = runtime
    cold = dict(base, YCXX_NO_DAEMON='1')
    daemon = dict(base, YCXX_DAEMON='require')
    envs = [('in-process', cold), ('daemon', daemon)]
    # Scratch directories are outside any repository: give them one scope, by marker.
    scope = Path(tempfile.mkdtemp(prefix='ycxx-scope-'))
    (scope / '.ycxx').write_text('')
    tempfile.tempdir = str(scope)

    try:
        # 1. The own suites, each test in each action.
        tests = sorted(p for sub in ('slice', 'smoke') for p in (own / sub).glob('*.cpp'))
        for t in tests:
            def setup(d, t=t):
                shutil.copy(t, d / t.name)
                if (t.parent / 'inputs').is_dir():
                    shutil.copytree(t.parent / 'inputs', d / 'inputs')
            for action in ([], ['-c'], ['-fsyntax-only'], ['-E']):
                compare(ycxx, f'{t.parent.name}/{t.name} {" ".join(action) or "link"}',
                        [*action, t.name, *(['-o', 'prog'] if not action else [])], setup, envs)

        # 2. One argument vector, two directories, two different local headers (§5.2): run both
        # through one daemon, each must equal its own cold compile.
        work = Path(tempfile.mkdtemp())
        for sub, value in (('a', 1), ('b', 2)):
            (work / sub).mkdir()
            (work / sub / 'v.h').write_text(f'#define V {value}\n')
            (work / sub / 'm.cpp').write_text('#include "v.h"\nint main() { return V; }\n')
        def build(sub, env, args=('-c', 'm.cpp', '-o', 'm.o')):
            """The answer and the object it wrote."""
            r = run(ycxx, list(args), work / sub, env)
            o = work / sub / 'm.o'
            data = o.read_bytes() if o.exists() else None
            if o.exists():
                o.unlink()
            return r, data
        for sub in ('a', 'b'):
            for args in (['-E', 'm.cpp'], ['-c', 'm.cpp', '-o', 'm.o']):
                want, got = build(sub, cold, args), build(sub, daemon, args)
                if want != got:
                    fail(f'same argv {args} in {sub}/: {got!r:.300} != cold {want!r:.300}')

        # 3. An edit to a header between two identical requests is seen at once (cookie sync,
        # §5.4), in -E and in the object: the engine session kept for an identical request must
        # not answer from cells that recorded only the main file.
        h = work / 'a' / 'v.h'
        for value in (3, 4, 5):
            h.write_text(f'#define V {value}\n')
            want = run(ycxx, ['-E', 'm.cpp'], work / 'a', cold)
            got = run(ycxx, ['-E', 'm.cpp'], work / 'a', daemon)
            if want != got or f'return {value};'.encode() not in got[1]:
                fail(f'edit to V={value}: {got!r:.300} != cold {want!r:.300}')
            want, got = build('a', cold), build('a', daemon)
            if want != got:
                fail(f'edit to V={value}, -c: the daemon\'s object differs from a cold compile')

        # A diagnostic in a header, after an edit that moves only its newlines: the same tokens and
        # offsets, a different line (the position is rendered from the header as an input, #93).
        (work / 'a' / 'w.h').write_text('/* a b */\n#warning x\n')
        (work / 'a' / 'w.cpp').write_text('#include "w.h"\nint main() { return 0; }\n')
        for text in ('/* a b */\n#warning x\n', '/* a\nb */\n#warning x\n', '/* a b */\n#warning x\n'):
            (work / 'a' / 'w.h').write_text(text)
            want = run(ycxx, ['-fsyntax-only', 'w.cpp'], work / 'a', cold)
            got = run(ycxx, ['-fsyntax-only', 'w.cpp'], work / 'a', daemon)
            if want != got:
                fail(f'a header diagnostic after a newline-only edit: {got!r:.300} != cold {want!r:.300}')

        # An identical request at an unchanged revision executes nothing (#96's example).
        build('a', daemon)
        build('a', daemon)
        status = subprocess.run([ycxx, 'ctl', 'status'], cwd=work, env=base, capture_output=True, text=True)
        fields = dict(line.split(': ', 1) for line in status.stdout.splitlines() if ': ' in line)
        if fields.get('last_executed') != '0':
            fail(f'a repeated request executed {fields.get("last_executed")} cells')

        status = subprocess.run([ycxx, 'ctl', 'status'], cwd=work, env=base, capture_output=True, text=True)
        fields = dict(line.split(': ', 1) for line in status.stdout.splitlines() if ': ' in line)
        if status.returncode != 0 or int(fields.get('revision', 0)) < 3 or int(fields.get('requests', 0)) == 0:
            fail(f'ctl status after edits: {status.stdout!r:.500} {status.stderr!r:.300}')

        # 4. Fallback: a runtime directory that cannot be used runs in-process, silently.
        bad = dict(base, YCXX_DAEMON='1', YCXX_RUNTIME_DIR=str(work / 'a' / 'v.h'))
        if run(ycxx, ['-E', 'm.cpp'], work / 'a', bad) != run(ycxx, ['-E', 'm.cpp'], work / 'a', cold):
            fail('fallback with an unusable runtime directory differs from in-process')
        if run(ycxx, ['-E', 'm.cpp'], work / 'a', dict(bad, YCXX_DAEMON='require'))[0] != 2:
            fail('YCXX_DAEMON=require with an unusable runtime directory is not an error')

        # 5. A daemon killed between requests leaves its socket behind; the next client finds it
        # stale, starts a new daemon, and gets the same answer.
        pid = int(fields.get('pid', 0))
        if pid:
            os.kill(pid, signal.SIGKILL)
            for _ in range(100):
                try:
                    os.kill(pid, 0)
                    time.sleep(0.05)
                except ProcessLookupError:
                    break
            if not Path(fields['socket']).exists():
                fail('the killed daemon left no socket to test with')
            got = run(ycxx, ['-E', 'm.cpp'], work / 'a', daemon)
            if got != run(ycxx, ['-E', 'm.cpp'], work / 'a', cold):
                fail(f'after a killed daemon: {got!r:.300}')
            again = subprocess.run([ycxx, 'ctl', 'status'], cwd=work, env=base, capture_output=True, text=True)
            if f'pid: {pid}\n' in again.stdout or again.returncode != 0:
                fail(f'no new daemon after the old one was killed: {again.stdout!r:.300}')
    finally:
        subprocess.run([ycxx, 'ctl', 'shutdown'], cwd=scope, env=base, capture_output=True)
        shutil.rmtree(scope, ignore_errors=True)
        shutil.rmtree(runtime, ignore_errors=True)

    if failures:
        print(f'daemon.compare: {len(failures)} failure(s)', file=sys.stderr)
        return 1
    print(f'daemon.compare: {len(tests)} tests x 4 actions, two directories, edits, fallback and a killed daemon: identical')
    return 0


if __name__ == '__main__':
    sys.exit(main())
