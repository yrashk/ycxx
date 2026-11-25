"""Incremental tests: a base state plus edits, each state checked against a cold run (DECISIONS §4.2).

The format (`*.incr`): a header of `//` directives, then the base state's files, then edits.

    // ACTION: syntax          what is compared: syntax (verdict and error locations, the
    //                         default), preprocess (-E output) or run (exit status and stdout)
    // MAIN: main.cpp          the translation unit (default main.cpp)
    // FEWER: 2 3              edits that must cost ycxx strictly less than a fresh daemon
    //                         (the default: no more)
    --- main.cpp               a file of the base state; its text follows, up to the next marker
    #include "a.h"
    int main() { return f(); }
    --- a.h
    inline int f() { return 0; }
    === edit                   an edit: each file listed replaces that file (or adds it) ...
    --- a.h
    inline int f() { return 1; }
    --- delete b.h             ... or deletes it
    === edit
    ...

For each state, the incremental result (the compiler having seen the previous states) must equal
a cold run on a fresh copy of that state. With ACTION: syntax the cold result must match the `//!`
error markers in the state's files (no markers: accepted); with run or preprocess every state
must build. The generators of edit sequences (random, mined from
git history, minimized; DECISIONS §4.2) produce states in the same shape: a list of
{path: text}.

The hook is incremental_results(): for Clang and GCC, which have no incremental mode, it edits
one directory in place and compiles it cold each time, which exercises the format and the
comparison. For ycxx it is the resident daemon (daemon design §8.2, #19): a private daemon per
test, on its own runtime directory and scope, sees each state as file changes, and answers the
action through `ycxx` as any build would. Beyond equality with the cold run, each state's request
is repeated, and the repeat must execute no engine cell; and an edited state must execute no more
cells than a fresh daemon does for that state from scratch.
"""
import os, shutil, signal, subprocess, tempfile, time
from pathlib import Path

from . import kinds


def parse(text):
    """[state0, state1, ...], each {relative path: text}."""
    states, files, cur, buf = [], {}, None, []

    def flush():
        if cur is not None:
            files[cur] = ''.join(buf)

    lines = text.splitlines(keepends=True)
    started = False
    for line in lines:
        if line.startswith('=== '):
            if line.split()[1:2] != ['edit']:
                raise ValueError(f'unknown section "{line.strip()}" (=== edit)')
            flush()
            cur, buf = None, []
            states.append(dict(files))
            started = True
        elif line.startswith('--- '):
            flush()
            buf = []
            words = line[4:].split()
            if words[:1] == ['delete'] and len(words) == 2:
                if not states:
                    raise ValueError('"--- delete" in the base state')
                if words[1] not in files:
                    raise ValueError(f'"--- delete {words[1]}": no such file in the state')
                files.pop(words[1])
                cur = None
            elif len(words) == 1:
                if Path(words[0]).is_absolute() or '..' in Path(words[0]).parts:
                    raise ValueError(f'file path must be relative and inside the test: {words[0]}')
                cur = words[0]
            else:
                raise ValueError(f'bad file line "{line.strip()}"')
            started = True
        elif cur is not None:
            buf.append(line)
        elif started and line.strip():
            raise ValueError(f'text outside a file: "{line.strip()}"')
    flush()
    states.append(dict(files))
    if not states[0]:
        raise ValueError('no base state')
    return states


def materialize(state, d, previous=None):
    """Writes state into d; with previous, only what changed (as an editor would)."""
    d.mkdir(parents=True, exist_ok=True)
    for p in (previous or {}):
        if p not in state:
            (d / p).unlink(missing_ok=True)
    for p, text in state.items():
        if previous is None or previous.get(p) != text:
            (d / p).parent.mkdir(parents=True, exist_ok=True)
            (d / p).write_text(text)


def placeholder(text, d):
    """Text with the state's directory written as <dir>, so two directories compare equal."""
    for p in sorted({str(d), os.path.realpath(d)}, key=len, reverse=True):
        text = text.replace(p, '<dir>')
    return text


def relative(path, d):
    """A diagnostic's file, relative to the state's directory when it is inside it."""
    real, root = os.path.realpath(path), os.path.realpath(d)
    return os.path.relpath(real, root) if real == root or real.startswith(root + os.sep) else real


def observe(c, action, main, d, ctx, tr, flags, timeout, label, env=None):
    """One state's result, comparable across directories (paths made relative to d). For ycxx
    against ycxx (`exact`) everything the compiler says and writes is compared, with the
    directory replaced by a placeholder: messages, raw -E output, the executable's bytes."""
    src = d / main
    exact = c.name == 'ycxx'
    if action == 'syntax':
        r = kinds.run(tr, f'{label} syntax', c.syntax(src, flags), timeout, cwd=d, env=env)
        if kinds.crashed(r.status):
            raise kinds.Failed(f'{label}: {c.name} crashed (exit {r.status})')
        errs = sorted((relative(f, d), l, col) for f, l, col in kinds.toolchain.errors_in(r.stdout + r.stderr, d))
        extra = (placeholder(r.stdout, d), placeholder(r.stderr, d)) if exact else ()
        return ('accept' if r.status == 0 else 'reject', tuple(errs), *extra), r
    if action == 'preprocess':
        r = kinds.run(tr, f'{label} -E', c.preprocess(src, flags), timeout, cwd=d, env=env, stdout_to=d.parent / f'{d.name}.i')
        if kinds.crashed(r.status):
            raise kinds.Failed(f'{label}: {c.name} crashed (exit {r.status})')
        if exact:
            return (r.status, placeholder(r.stdout, d), placeholder(r.stderr, d)), r
        out = [l for l in kinds.normalize_pp(r.stdout) if str(d) not in l]
        return (r.status, tuple(out)), r
    # The executable has the same name on both sides (its directory's parent differs), so its
    # bytes compare.
    ok, st, out = kinds.build_and_run(c, src, ctx, tr, flags, timeout, d.parent, label=f'{label} ', env=env)
    exe = d.parent / f'{c.name}.out'
    if not ok:
        return ('reject',), None
    return (('accept', st, out, exe.read_bytes()) if exact else ('accept', st, out)), None


def in_process():
    """The environment of a cold compile: never through a daemon, whatever the caller's is."""
    env = {k: v for k, v in os.environ.items() if k != 'YCXX_DAEMON'}
    env['YCXX_NO_DAEMON'] = '1'
    return env


class Daemon:
    """A private ycxx daemon (daemon design §8.2): its own short runtime directory (a socket path
    holds 104 bytes on macOS) and its own scope, a directory marked `.ycxx`, so neither the
    repository nor another test shares it. `env` routes `ycxx` through it. `expect(n)` checks that
    it is still the daemon that answered the first request and has answered n requests in all: a
    client restarts a daemon that died, and the fresh one would answer like a cold compile.
    `close` shuts it down, and kills it if it does not exit (a request left hanging)."""

    def __init__(self, exe, scope):
        self.exe = exe
        self.runtime = Path(tempfile.mkdtemp(prefix='yi', dir='/tmp'))
        os.chmod(self.runtime, 0o700)
        self.scope = scope
        scope.mkdir(parents=True, exist_ok=True)
        (scope / '.ycxx').write_text('')
        base = {k: v for k, v in os.environ.items() if k not in ('YCXX_DAEMON', 'YCXX_NO_DAEMON', 'YCXX_ROOT', 'YCXX_NO_WATCH')}
        base['YCXX_RUNTIME_DIR'] = str(self.runtime)
        base['YCXX_DAEMON_IDLE_EXIT'] = '300'  # if the driver itself is killed
        self.base = base
        self.env = dict(base, YCXX_DAEMON='require')
        self.pid = None
        self.regions = 0  # pp regions computed so far

    def status(self):
        """`ycxx ctl status` as {field: value}."""
        p = subprocess.run([self.exe, 'ctl', 'status'], cwd=self.scope, env=self.base, capture_output=True,
                           text=True, timeout=60)
        if p.returncode != 0:
            raise kinds.Failed(f'ycxx ctl status failed (exit {p.returncode}): {p.stderr.strip()[:300]}')
        return dict(line.split(': ', 1) for line in p.stdout.splitlines() if ': ' in line)

    def expect(self, requests):
        """The work of the last request: (engine cells executed, pp regions computed), after
        checking that this daemon answered every request so far."""
        st = self.status()
        pid = int(st.get('pid', 0))
        if self.pid is None:
            self.pid = pid
        if pid != self.pid or int(st.get('requests', -1)) != requests:
            raise kinds.Failed(f'the daemon was replaced or missed a request: pid {pid} (first {self.pid}), '
                               f'{st.get("requests")} requests answered, {requests} sent')
        regions = int(st.get('regions_computed', 0))
        work = (int(st.get('last_executed', -1)), regions - self.regions)
        self.regions = regions
        return work

    def close(self):
        try:
            subprocess.run([self.exe, 'ctl', 'shutdown'], cwd=self.scope, env=self.base, capture_output=True, timeout=30)
        except subprocess.TimeoutExpired:
            pass
        finally:
            if self.pid:
                for _ in range(50):
                    try:
                        os.kill(self.pid, 0)
                    except ProcessLookupError:
                        break
                    time.sleep(0.1)
                else:
                    try:
                        os.kill(self.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
            shutil.rmtree(self.runtime, ignore_errors=True)


def daemon_results(c, states, action, main, ctx, tr, flags, timeout, work, fewer=()):
    """ycxx's side: one daemon sees every state in one directory, edited in place. The work a
    request did is its engine cells executed, and for -E (which runs no engine query) the pp
    regions it computed."""
    tr.note('# incremental side: a private ycxx daemon, each state written as file changes')
    dm = Daemon(c.exe, work / 'scope')
    d = dm.scope / 'incremental'
    out, done, prev, sent = [], [], None, 0
    measure = 'pp regions computed' if action == 'preprocess' else 'engine cells executed'
    pick = (lambda w: w[1]) if action == 'preprocess' else (lambda w: w[0])
    try:
        for i, s in enumerate(states):
            materialize(s, d, prev)
            prev = s
            result = observe(c, action, main, d, ctx, tr, flags, timeout, f'inc{i}', env=dm.env)[0]
            sent += 1
            done.append(pick(dm.expect(sent)))
            again = observe(c, action, main, d, ctx, tr, flags, timeout, f'inc{i} again', env=dm.env)[0]
            sent += 1
            repeat = pick(dm.expect(sent))
            if again != result:
                raise kinds.Failed(f'state {i}: a repeated request through the daemon answered differently')
            if repeat != 0:
                raise kinds.Failed(f'state {i}: a repeated request did work ({repeat} {measure}; want 0)')
            out.append(result)
    finally:
        dm.close()
    tr.check(f'every state answered the same when repeated, doing no work; {measure} per state: {done}')
    # The same states, each from scratch in a fresh daemon: an edit may not cost more than that,
    # and a state the test lists in FEWER must cost strictly less (daemon design §8.2).
    fresh_work = []
    for i, s in enumerate(states[1:], 1):
        fresh = Daemon(c.exe, work / f'scope-cold-{i}')
        try:
            materialize(s, fresh.scope / 'cold')
            observe(c, action, main, fresh.scope / 'cold', ctx, tr, flags, timeout, f'fresh{i}', env=fresh.env)
            fresh_work.append(pick(fresh.expect(1)))
        finally:
            fresh.close()
        if done[i] > fresh_work[-1] or (i in fewer and done[i] >= fresh_work[-1]):
            raise kinds.Failed(f'state {i}: the edit did {done[i]} ({measure}), a fresh daemon {fresh_work[-1]}'
                               + (' (FEWER: it must be strictly less)' if i in fewer else ''))
    if len(states) > 1:
        tr.check(f'no edited state did more than a fresh daemon for it ({measure}: {fresh_work})'
                 + (f'; strictly less in states {sorted(fewer)}' if fewer else ''))
    return out


def incremental_results(c, states, action, main, ctx, tr, flags, timeout, work, fewer=()):
    """The hook: the result of each state, as compiler c sees it after the earlier states."""
    if c.name == 'ycxx':
        return daemon_results(c, states, action, main, ctx, tr, flags, timeout, work, fewer)
    tr.note(f'# incremental side: {c.name} has no incremental mode; one directory edited in place, '
            'compiled cold after each edit')
    d = work / 'incremental'
    out, prev = [], None
    for i, s in enumerate(states):
        materialize(s, d, prev)
        prev = s
        out.append(observe(c, action, main, d, ctx, tr, flags, timeout, f'inc{i}')[0])
    return out


def run_test(test, ctx, tr, flags, timeout, work):
    action = test.one('ACTION', 'syntax')
    feature = {'syntax': 'syntax', 'preprocess': 'preprocess', 'run': 'run'}.get(action)
    if not feature:
        raise kinds.Unresolved(f'unknown ACTION "{action}" (syntax, preprocess, run)')
    main = test.one('MAIN', 'main.cpp')
    try:
        states = parse(test.path.read_text())
    except ValueError as e:
        raise kinds.Unresolved(f'{test.id}: {e}')
    if any(main not in s for s in states):
        raise kinds.Unresolved(f'MAIN {main} is missing from a state')
    kinds.need(ctx.cut, feature)
    kinds.need(ctx.cut, 'incremental')
    try:
        fewer = {int(n) for n in test.one('FEWER', '').split()}
    except ValueError:
        raise kinds.Unresolved(f'FEWER takes state numbers: "{test.one("FEWER", "")}"')
    if any(n < 1 or n >= len(states) for n in fewer):
        raise kinds.Unresolved(f'FEWER names a state that is not an edit: {sorted(fewer)}')
    tr.check(f'{len(states)} states (the base and {len(states) - 1} edits), action {action}')
    inc = incremental_results(ctx.cut, states, action, main, ctx, tr, flags, timeout, work, fewer)
    for i, s in enumerate(states):
        d = work / f'cold-{i}'
        materialize(s, d)
        cold, r = observe(ctx.cut, action, main, d, ctx, tr, flags, timeout, f'cold{i}', env=in_process())
        if inc[i] != cold:
            tr.note(f'incremental: {inc[i]!r:.300}\ncold:        {cold!r:.300}')
            raise kinds.Failed(f'state {i}: the incremental result differs from the cold run')
        if action == 'syntax':
            expected = kinds.expected_errors([d / p for p in s], compiler=ctx.cut.name)
            problems, _ = kinds.verdict_problems(ctx, r, expected, d)
            if problems:
                raise kinds.Failed(f'state {i} (cold): ' + '; '.join(problems))
        elif cold[0] == 'reject' or (action == 'preprocess' and cold[0] != 0):
            raise kinds.Failed(f'state {i} (cold): did not {"build" if action == "run" else "preprocess"}; '
                               f'a {action} action needs every state to')
        tr.check(f'state {i}: incremental equals cold ({cold[0]})')
