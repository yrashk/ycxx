"""Incremental tests: a base state plus edits, each state checked against a cold run (DECISIONS §4.2).

The format (`*.incr`): a header of `//` directives, then the base state's files, then edits.

    // ACTION: syntax          what is compared: syntax (verdict and error locations, the
    //                         default), preprocess (-E output) or run (exit status and stdout)
    // MAIN: main.cpp          the translation unit (default main.cpp)
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
a cold run on a fresh copy of that state, and the cold result must match the `//!` error markers
in the state's files (no markers: accepted). The generators of edit sequences (random, mined from
git history, minimized; DECISIONS §4.2) produce states in the same shape: a list of
{path: text}.

The hook is incremental_results(): for Clang and GCC, which have no incremental mode, it edits
one directory in place and compiles it cold each time, which exercises the format and the
comparison. For ycxx it is the daemon's incremental protocol, which #19 (the incremental
harness) plugs in here; until then ycxx lacks the "incremental" feature and the tests are
UNSUPPORTED.
"""
import os, shutil
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
                files.pop(words[1], None)
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


def observe(c, action, main, d, ctx, tr, flags, timeout, label):
    """One state's result, comparable across directories (paths made relative to d)."""
    src = d / main
    if action == 'syntax':
        r = kinds.run(tr, f'{label} syntax', c.syntax(src, flags), timeout, cwd=d)
        errs = sorted((os.path.relpath(f, os.path.realpath(d)), l, col)
                      for f, l, col in kinds.toolchain.errors_in(r.stdout + r.stderr, d))
        return ('accept' if r.status == 0 else 'reject', tuple(errs)), r
    if action == 'preprocess':
        r = kinds.run(tr, f'{label} -E', c.preprocess(src, flags), timeout, cwd=d, stdout_to=d.parent / f'{d.name}.i')
        out = [l for l in kinds.normalize_pp(r.stdout) if str(d) not in l]
        return (r.status, tuple(out)), r
    ok, st, out = kinds.build_and_run(c, src, ctx, tr, flags, timeout, d.parent, tag=f'-{d.name}', label=f'{label} ')
    return (('accept', st, out) if ok else ('reject',)), None


def incremental_results(c, states, action, main, ctx, tr, flags, timeout, work):
    """The hook: the result of each state, as compiler c sees it after the earlier states."""
    if c.name == 'ycxx':
        # #19 replaces this with the daemon's protocol: load states[0], apply each edit as a
        # file change, ask for the action's result after each.
        raise kinds.Unsupported('the ycxx incremental protocol is not wired yet (#19)')
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
    tr.check(f'{len(states)} states (the base and {len(states) - 1} edits), action {action}')
    inc = incremental_results(ctx.cut, states, action, main, ctx, tr, flags, timeout, work)
    for i, s in enumerate(states):
        d = work / f'cold-{i}'
        materialize(s, d)
        cold, r = observe(ctx.cut, action, main, d, ctx, tr, flags, timeout, f'cold{i}')
        if action == 'syntax':
            expected = kinds.expected_errors([d / p for p in s])
            problems, _ = kinds.verdict_problems(ctx, r, expected, d)
            if problems:
                raise kinds.Failed(f'state {i} (cold): ' + '; '.join(problems))
        if inc[i] != cold:
            tr.note(f'incremental: {inc[i]!r:.300}\ncold:        {cold!r:.300}')
            raise kinds.Failed(f'state {i}: the incremental result differs from the cold run')
        tr.check(f'state {i}: incremental equals cold ({cold[0]})')
