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
- `--trace-engine=json`: every stderr line is a JSON object (events with their values, the
  diagnostics and exit status, the summary), for tools and agents.
- Trace details show the compiler's file, line and column for source errors (#404).
- Memory (#274): events carry their bytes in the schema docs/TRACE.md gives, the summary its
  totals and kinds, the stream a memory line, and `ctl status` the daemon's memory.
- `ycxx cxx --watch --trace-engine` builds again when a header the build read changes, and only
  then: an unrelated file changes nothing.
"""
import json, os, subprocess, sys, tempfile, threading, time, shutil
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
        tail = first.stderr.rstrip().splitlines()[-3:]
        check(len(tail) == 3 and tail[0].startswith('trace: ') and tail[1].startswith('memory: ') and tail[2].startswith('by kind: '),
              f'the summary ends the stream, with memory and kinds: {tail}')
        check(any(' alloc ' in l and 'retains ' in l for l in lines(first.stderr)), 'executed cells show what they allocate and retain')

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

        js = cxx('--trace-engine=json', 'a.cpp', '-o', 'a')
        check(js.returncode == 0, f'a JSON trace: {js.stderr[-300:]}')
        try:
            objs = [json.loads(l) for l in js.stderr.splitlines() if l.strip()]
        except ValueError as e:
            objs = []
            fail(f'every line of a JSON trace is JSON: {e}: {js.stderr[:300]}')
        events = [o for o in objs if 'kind' in o]
        check(any(o['kind'] == 'Build' and o['outcome'] in ('reused', 'executed') for o in events), 'the JSON trace has the root')
        check(all({'id', 'parent', 'depth', 'outcome', 'duration_ns', 'worker'} <= o.keys() for o in events), 'each event has its fields')
        check(sum('summary' in o for o in objs) == 1 and sum('exit_status' in o for o in objs) == 1, 'one summary, one exit status')

        def traced_error(file, where, message, kinds, *options):
            result = cxx('--trace-engine=json', *options, file)
            records = [json.loads(l) for l in result.stderr.splitlines() if l.strip()]
            expected = f'{where}: error: {message}'
            check(result.returncode == 1 and any(expected in o.get('diagnostics', '') for o in records),
                  f'the compiler reports {expected}')
            for kind in kinds:
                details = [o['errors'] for o in records if o.get('kind') == kind and 'errors' in o]
                check(bool(details) and all(f'{where}: {message}' in text for text in details),
                      f'{kind} details report {where}: {message}; got {details}')

        diagnostic = scope / 'diagnostic.cpp'
        missing = "use of undeclared identifier 'missing'"
        diagnostic.write_bytes(b'/*   */\r\nint f() {\r\n  return missing;\r\n}\r\n')
        traced_error('./diagnostic.cpp', './diagnostic.cpp:3:10', missing, ('Build', 'TuFront'), '-fsyntax-only')
        # Same token byte offsets after this edit, but two more lines in the compiler's source.
        diagnostic.write_bytes(b'/*\n\n */\r\nint f() {\r\n  return missing;\r\n}\r\n')
        traced_error('./diagnostic.cpp', './diagnostic.cpp:5:10', missing, ('Build', 'TuFront'), '-fsyntax-only')
        header = scope / 'diagnostic.h'
        diagnostic.write_text('#include "diagnostic.h"\n')
        header.write_text('\ninline int f() {\n  return missing;\n}\n')
        traced_error('diagnostic.cpp', './diagnostic.h:3:10', missing, ('Build', 'TuFront'), '-fsyntax-only')
        header.write_text('\n#include "no-such-header.h"\n')
        traced_error('diagnostic.cpp', './diagnostic.h:2:10', 'file not found', ('Build', 'TuFront', 'Pp'), '-fsyntax-only')
        # A malformed pasted token can be wider than the header text in which it originated.
        header.write_text('#define A 0')
        diagnostic.write_text('#include "diagnostic.h"\n#define P(a,b) a##b\n#define Q(a,b) P(a,b)\n'
                              'int k=Q(A,777777777777777777777789);\n')
        traced_error('diagnostic.cpp', './diagnostic.h:1:12', 'invalid preprocessing number',
                     ('Build', 'TuFront', 'Pp'), '-fsyntax-only')
        diagnostic.write_text('#include "diagnostic.h"\n')
        definition = 'double f(double x, ...) {\n  return x;\n}\n'
        abi_error = 'x86-64 System V ABI: floating-point parameters and results of variadic functions are unsupported'
        header.write_text('/*   */\n' + definition)
        traced_error('diagnostic.cpp', './diagnostic.h:2:25', abi_error,
                     ('Build', 'Tu', 'Lower'), '--target=x86_64-pc-linux-gnu', '-c')
        header.write_text('/*\n\n */\n' + definition)
        traced_error('diagnostic.cpp', './diagnostic.h:4:25', abi_error,
                     ('Build', 'Tu', 'Lower'), '--target=x86_64-pc-linux-gnu', '-c')
        absent = cxx('--trace-engine=json', '-fsyntax-only', 'absent.cpp')
        records = [json.loads(l) for l in absent.stderr.splitlines() if l.strip()]
        check(absent.returncode == 1 and any(o.get('kind') == 'Build' and o.get('errors') == 'cannot read a source file\n' for o in records),
              'a trace still shows a readable error when the source file is absent')

        st = ctl('status')
        fields = dict(l.split(': ', 1) for l in st.stdout.splitlines() if ': ' in l and not l.startswith('retained_by_kind'))
        check(st.returncode == 0 and all(int(fields.get(f, '-1')) > 0 for f in ('memory_current', 'memory_peak', 'memory_allocated', 'allocations')),
              f'ctl status has the daemon\'s memory: {st.stdout[-600:]}')
        check(int(fields.get('memory_retained', '0')) > 0 and int(fields.get('memory_current', '0')) - int(fields.get('memory_retained', '0'))
              == int(fields.get('memory_unattributed', '-1')), 'retained and unattributed add up to the current bytes')
        check(any(l.startswith('retained_by_kind: Pp ') for l in st.stdout.splitlines()), f'and what each kind retains: {st.stdout[-600:]}')
        src.write_text(SOURCE.replace('x * x', 'x * x * 3'))
        js2 = cxx('--trace-engine=json', 'a.cpp', '-o', 'a')
        values = [json.loads(l) for l in js2.stderr.splitlines() if '"value"' in l]
        check(any(v['kind'] == 'FunctionIr' and 'mul' in v['value'] for v in values), 'a re-run function shows its IR')
        # The memory schema (docs/TRACE.md), on a build that re-runs cells.
        objs = [json.loads(l) for l in js2.stderr.splitlines() if l.strip()]
        events = [o for o in objs if 'kind' in o]
        mem_fields = ('alloc_bytes', 'allocs', 'peak_bytes', 'retained_bytes', 'incl_alloc_bytes', 'incl_retained_bytes')
        check(all(all(isinstance(o.get(f), int) for f in mem_fields) for o in events), 'each event has its memory fields, as integers')
        check(all(o['incl_alloc_bytes'] >= o['alloc_bytes'] >= 0 and o['allocs'] >= 0 for o in events), 'a subtree allocates at least its root')
        check(all(o['alloc_bytes'] == 0 for o in events if o['outcome'] == 'reused'), 'a reuse allocates nothing')
        summary = next((o['summary'] for o in objs if 'summary' in o), {})
        memory = summary.get('memory', {})
        check(all(isinstance(memory.get(f), int) for f in ('alloc_bytes', 'allocs', 'retained_bytes', 'process_current_bytes', 'process_peak_bytes')),
              f'the summary has the build\'s memory: {memory}')
        check(memory.get('process_peak_bytes', 0) >= memory.get('process_current_bytes', 1) > 0, 'the process peak is at least its current bytes')
        check(memory.get('alloc_bytes', 0) > 0 and memory.get('alloc_bytes') == sum(o['alloc_bytes'] for o in events),
              'the summary sums the events\' allocations')
        kinds = summary.get('kinds', [])
        check(all({'kind', 'executions', 'alloc_bytes', 'allocs', 'peak_bytes', 'retained_bytes'} <= k.keys() for k in kinds),
              f'each kind has its fields: {kinds[:2]}')
        check(sum(k['alloc_bytes'] for k in kinds) == memory.get('alloc_bytes'), 'the kinds add up to the build')
        check([k['alloc_bytes'] for k in kinds] == sorted((k['alloc_bytes'] for k in kinds), reverse=True), 'most allocated first')
        src.write_text(EDITED)

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

        check(wait_for('ycxx: watching '), f'--watch builds once and waits: {"".join(log)[-400:]}')
        check(any('ycxx: watching' in l and 'k.h' in l for l in log), 'it lists the header it watches')
        (scope / 'unrelated.txt').write_text('x')
        time.sleep(2)
        check(not any('── build 2' in l for l in log), 'a file no build read starts no build')
        (scope / 'k.h').write_text('inline int k() { return 0; }\n')
        check(wait_for('── build 2'), f'a header the build read starts a build: {"".join(log)[-400:]}')
        check(wait_for('trace: ', times=2), 'the rebuild is traced')
        second = ''.join(log[next(i for i, l in enumerate(log) if '── build 2' in l):])
        check('Lower(k) executed' in second or 'FunctionIr(k) executed' in second, f'k is built again: {second[:600]}')
        # An edit that another client's build applies first still starts a build here.
        check(wait_for('ycxx: watching ', times=2), 'the rebuild waits again')
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
