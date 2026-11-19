"""Running one test of each kind (docs/TESTING.md, "Test kinds").

  accept       must compile (-fsyntax-only), with no error
  reject       must be rejected with errors exactly at the `//!` markers' locations (wording is
               never compared)
  preprocess   -E output equal to the oracle's (Clang by default), after normalization
  run          builds and runs; exit status and, when given, stdout
  diff         the compiler under test agrees with Clang 23 and GCC 16 on the verdict, the
               program's behaviour or -E output; the oracles must agree with each other
  incremental  a base state plus edits; each state's incremental result equals a cold run
               (tests/driver/incremental.py)

execute() returns (result code, transcript, steps). Codes are lit's: PASS, FAIL, XFAIL, XPASS,
UNSUPPORTED, UNRESOLVED (the test itself is malformed, or the oracles disagree), TIMEOUT.
"""
import difflib, os, re, shutil, subprocess, sys
from dataclasses import dataclass
from pathlib import Path

from . import incremental, toolchain
from .discover import markers_in
from .transcript import Transcript


class Unsupported(Exception): pass
class Unresolved(Exception): pass
class Failed(Exception): pass
class Crashed(Failed): pass  # never expected: an XFAIL does not cover a crash
class TimedOut(Exception): pass


@dataclass
class Ctx:
    repo: Path
    cut: toolchain.Compiler      # the compiler under test
    compilers: dict              # name -> Compiler
    oracle: str                  # default oracle of preprocess tests
    work: Path                   # build/test-work
    timeout: int


KIND_FEATURE = {'accept': 'syntax', 'reject': 'syntax', 'preprocess': 'preprocess', 'run': 'run'}
PLATFORM = {'darwin': 'darwin', 'linux': 'linux'}.get(sys.platform, sys.platform)


def run(tr, step, cmd, timeout, **kw):
    r = tr.run(step, cmd, timeout=timeout, **kw)
    if r.status is None:
        raise TimedOut(f'{step} timed out after {timeout}s')
    return r


def crashed(status):
    """A signal or an exit status other than 0 and 1: an internal error, never a verdict."""
    return status is not None and (status < 0 or status > 1)


def need(compiler, feature):
    why = compiler.lacks(feature)
    if why:
        raise Unsupported(why)


def rel(ctx, path):
    r = os.path.relpath(path, ctx.repo)
    return str(path) if r.startswith('..') else r


# --- accept / reject --------------------------------------------------------------------------

def expected_errors(files):
    """{(realpath, line, col or None)} from the `//!` markers of files (paths)."""
    return {(os.path.realpath(f), line, col) for f in files
            for line, col in markers_in(Path(f).read_text(encoding='utf-8', errors='replace'))}


QUOTED_INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)


def with_headers(path):
    """The test file and the existing headers it includes with `#include "..."`, relative to it:
    their `//!` markers expect errors located in the header."""
    files = [path]
    for name in QUOTED_INCLUDE.findall(Path(path).read_text(encoding='utf-8', errors='replace')):
        h = Path(path).parent / name
        if h.is_file():
            files.append(h)
    return files


def verdict_problems(ctx, r, expected, cwd):
    """What is wrong with a syntax run r given the expected error locations ([] when right)."""
    actual = toolchain.errors_in(r.stdout + r.stderr, cwd)
    show = lambda f, l, c: f'{rel(ctx, f)}:{l}' + (f':{c}' if c else '')
    problems = []
    if crashed(r.status):
        problems.append(f'crashed (exit {r.status}): a rejection is exit 1 with diagnostics')
    for f, l, c in sorted(expected, key=lambda e: (e[0], e[1], e[2] or 0)):
        if not any(af == f and al == l and (c is None or ac == c) for af, al, ac in actual):
            problems.append(f'expected an error at {show(f, l, c)}: none reported')
    for af, al, ac in actual:
        if not any(f == af and l == al and (c is None or c == ac) for f, l, c in expected):
            problems.append(f'unexpected error at {show(af, al, ac)}')
    if expected and r.status == 0:
        problems.append('accepted (exit 0), expected to be rejected')
    if not expected and r.status != 0 and not actual:
        problems.append(f'exit {r.status} without an error diagnostic')
    if expected and r.status != 0 and not actual:
        problems.append(f'rejected (exit {r.status}) but no "file:line:col: error:" diagnostic to locate')
    return problems, actual


def run_verify(test, ctx, tr, flags, timeout):
    need(ctx.cut, 'syntax')
    expected = expected_errors(with_headers(test.path))
    if test.kind == 'accept' and expected:
        raise Unresolved('an accept test has //! error markers (make it a .reject.cpp)')
    if test.kind == 'reject' and not expected:
        raise Unresolved('a reject test needs at least one //! error marker: where must it fail?')
    r = run(tr, 'syntax', ctx.cut.syntax(test.path, flags), timeout, cwd=test.path.parent)
    problems, actual = verdict_problems(ctx, r, expected, test.path.parent)
    tr.check(f'{len(expected)} error location(s) expected, {len(actual)} error(s) reported')
    if problems:
        raise (Crashed if crashed(r.status) else Failed)('; '.join(problems))


# --- preprocess -------------------------------------------------------------------------------

def normalize_pp(text, exact=False):
    """-E output for comparison: without line markers and blank lines, each line's whitespace
    runs collapsed to one space (Clang and GCC space tokens differently); `exact` keeps all but
    the line markers."""
    out = []
    for line in text.splitlines():
        s = line.strip()
        if s.startswith('#') and (s[1:].lstrip()[:1].isdigit() or s[1:].lstrip().startswith('line ')):
            continue
        if exact:
            out.append(line)
        elif s:
            out.append(' '.join(s.split()))
    return out


def preprocessed(c, test, ctx, tr, flags, timeout, work, exact):
    out = work / f'{c.name}.i'
    r = run(tr, f'{c.name} -E', c.preprocess(test.path, flags), timeout, cwd=test.path.parent, stdout_to=out)
    return r.status, normalize_pp(r.stdout, exact)


def diff_lines(a, b, an, bn, limit=60):
    d = list(difflib.unified_diff(a, b, an, bn, lineterm='', n=2))
    return '\n'.join(d[:limit]) + (f'\n... ({len(d) - limit} more diff lines)' if len(d) > limit else '')


def run_preprocess(test, ctx, tr, flags, timeout, work):
    need(ctx.cut, 'preprocess')
    oracle = ctx.compilers.get(test.one('ORACLE', ctx.oracle))
    if oracle is None:
        raise Unresolved(f'unknown ORACLE {test.one("ORACLE")}')
    need(oracle, 'preprocess')
    exact = test.one('PP-COMPARE') == 'exact'
    st, mine = preprocessed(ctx.cut, test, ctx, tr, flags, timeout, work, exact)
    ost, theirs = preprocessed(oracle, test, ctx, tr, flags, timeout, work, exact)
    if ost != 0:
        raise Unresolved(f'the oracle {oracle.name} failed to preprocess it (exit {ost})')
    if st != 0:
        raise Failed(f'{ctx.cut.name} -E exit {st}')
    tr.check(f'-E output vs {oracle.name}: {len(mine)} and {len(theirs)} lines'
             f' ({"exact" if exact else "normalized whitespace, no line markers"})')
    if mine != theirs:
        tr.note(diff_lines(theirs, mine, oracle.name, ctx.cut.name))
        raise Failed(f'-E output differs from {oracle.name}')


# --- run --------------------------------------------------------------------------------------

def expected_stdout(test):
    side = test.path.with_name(test.path.name + '.stdout')
    if side.is_file():
        return side.read_text()
    if 'STDOUT' in test.directives:
        return ''.join(l + '\n' for l in test.directives['STDOUT'])
    return None


def build_and_run(c, src, ctx, tr, flags, timeout, work, tag='', label='', env=None):
    """(compiled, exit status, stdout) of src built by c (in env, when given) and run."""
    exe = work / f'{c.name}{tag}.out'
    r = run(tr, f'{label}build', c.build(src, exe, flags), timeout, cwd=src.parent, env=env)
    if crashed(r.status):
        why = f'{c.name} crashed building {src.name} (exit {r.status})'
        raise Unresolved(why) if c.name in toolchain.ORACLES and c is not ctx.cut else Failed(why)
    if r.status != 0:
        return False, r.status, ''
    r = run(tr, f'{label}run', [exe], timeout, cwd=work)
    return True, r.status, r.stdout


def peer_compiler(test, ctx):
    """The compiler of a test's PEER files: the oracle Clang, or Apple Clang with
    `PEER-WITH: apple-clang`, for `-fclass-call-abi=apple` (DECISIONS §1.6)."""
    who = test.one('PEER-WITH', 'clang').strip()
    if who == 'clang':
        clang = ctx.compilers['clang']
        need(clang, 'run')
        return 'clang', clang.exe
    if who == 'apple-clang':
        r = subprocess.run(['xcrun', '--find', 'clang++'], capture_output=True, text=True) if sys.platform == 'darwin' else None
        if r is None or r.returncode != 0 or not r.stdout.strip():
            raise Unresolved('PEER-WITH: apple-clang needs Apple Clang (xcrun clang++)')
        return 'apple-clang', r.stdout.strip()
    raise Unresolved(f'unknown PEER-WITH: {who}')


def peer_objects(test, ctx, tr, timeout, work):
    """The test's PEER files, each compiled by the peer compiler to an object linked with the
    test: a mixed-object ABI test (DECISIONS §5.4)."""
    objs = []
    for name in (v.strip() for v in test.directives.get('PEER', [])):
        who, exe = peer_compiler(test, ctx)
        obj = Path(work) / (Path(name).stem + '.o')
        r = run(tr, f'{who} -c {name}', [exe, '-std=c++26', '-fno-exceptions', '-c', name, '-o', str(obj)], timeout, cwd=test.path.parent)
        if r.status != 0:
            raise Unresolved(f'{who} could not compile the peer {name} (exit {r.status})')
        objs.append(str(obj))
    return objs


def run_run(test, ctx, tr, flags, timeout, work):
    need(ctx.cut, 'run')
    flags = [*flags, *peer_objects(test, ctx, tr, timeout, work)]
    ok, st, out = build_and_run(ctx.cut, test.path, ctx, tr, flags, timeout, work)
    if not ok:
        raise Failed(f'did not compile (exit {st})')
    want = int(test.one('EXIT', '0'))
    stdout = expected_stdout(test)
    tr.check(f'exit status {st}, expected {want}' + ('' if stdout is None else '; stdout compared'))
    if st != want:
        raise Failed(f'exit status {st}, expected {want}')
    if stdout is not None and out != stdout:
        tr.note(diff_lines(stdout.splitlines(), out.splitlines(), 'expected', 'actual'))
        raise Failed('stdout differs from the expected output')


# --- diff -------------------------------------------------------------------------------------

DIFF_FEATURE = {'run': 'run', 'verdict': 'syntax', 'preprocess': 'preprocess'}


def observe(c, mode, test, ctx, tr, flags, timeout, work):
    """What compiler c makes of the test, as a comparable value."""
    if mode == 'verdict':
        r = run(tr, f'{c.name} syntax', c.syntax(test.path, flags), timeout, cwd=test.path.parent)
        return ('crash', r.status) if crashed(r.status) else ('accept',) if r.status == 0 else ('reject',)
    if mode == 'preprocess':
        st, lines = preprocessed(c, test, ctx, tr, flags, timeout, work, False)
        return ('exit', st, tuple(lines) if st == 0 else ())
    ok, st, out = build_and_run(c, test.path, ctx, tr, flags, timeout, work, label=f'{c.name} ')
    return ('accept', st, out) if ok else ('reject',)


def show(v):
    if v[0] == 'crash':
        return f'crashed (exit {v[1]})'
    if v[0] == 'reject' or v == ('accept',):
        return v[0] + 'ed'
    if v[0] == 'exit':
        return f'-E exit {v[1]}, {len(v[2])} lines'
    return f'accepted, exit {v[1]}, stdout {v[2]!r:.80}'


def run_diff(test, ctx, tr, flags, timeout, work):
    mode = test.one('DIFF', 'run')
    if mode not in DIFF_FEATURE:
        raise Unresolved(f'unknown DIFF mode "{mode}" (run, verdict, preprocess)')
    need(ctx.cut, DIFF_FEATURE[mode])
    for o in toolchain.ORACLES:
        need(ctx.compilers[o], DIFF_FEATURE[mode])
    seen = {}
    for name in dict.fromkeys([ctx.cut.name, *toolchain.ORACLES]):
        tr.note(f'# {name}')
        seen[name] = observe(ctx.compilers[name], mode, test, ctx, tr, flags, timeout, work)
    for name, v in seen.items():
        tr.check(f'{name}: {show(v)}')
    if seen['clang'] != seen['gcc']:
        raise Unresolved(f'the oracles disagree: clang {show(seen["clang"])}; gcc {show(seen["gcc"])}')
    if seen[ctx.cut.name] != seen['clang']:
        raise Failed(f'{ctx.cut.name} {show(seen[ctx.cut.name])}; the oracles {show(seen["clang"])}')


# --- one test ---------------------------------------------------------------------------------

def xfail_reason(test, cut):
    for v in test.directives.get('XFAIL', []):
        who, _, why = v.partition(' ')
        if who == '*' or cut in who.split(','):
            return why.strip() or '(no reason given)'
    return None


def execute(test, ctx):
    tr = Transcript()
    work = ctx.work / test.id
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    flags = ' '.join(test.directives.get('FLAGS', [])).split()
    timeout = int(test.one('TIMEOUT', ctx.timeout))
    tr.note(f'# {test.kind} test {test.id} with {ctx.cut.name}')
    try:
        for req in ' '.join(test.directives.get('REQUIRES', [])).replace(',', ' ').split():
            if req in toolchain.FEATURES:
                need(ctx.cut, req)
            elif req in ('darwin', 'linux'):
                if req != PLATFORM:
                    raise Unsupported(f'requires {req}')
            else:
                raise Unresolved(f'unknown REQUIRES "{req}"')
        if test.kind in ('accept', 'reject'):
            run_verify(test, ctx, tr, flags, timeout)
        elif test.kind == 'preprocess':
            run_preprocess(test, ctx, tr, flags, timeout, work)
        elif test.kind == 'run':
            run_run(test, ctx, tr, flags, timeout, work)
        elif test.kind == 'diff':
            run_diff(test, ctx, tr, flags, timeout, work)
        elif test.kind == 'incremental':
            incremental.run_test(test, ctx, tr, flags, timeout, work)
        code = 'PASS'
    except Unsupported as e:
        code = 'UNSUPPORTED'
        tr.note(f'unsupported: {e}')
    except Unresolved as e:
        code = 'UNRESOLVED'
        tr.note(f'UNRESOLVED: {e}')
    except Crashed as e:
        code = 'CRASH'  # becomes FAIL below, never XFAIL
        tr.note(f'FAIL: {e}')
    except Failed as e:
        code = 'FAIL'
        tr.note(f'FAIL: {e}')
    except TimedOut as e:
        code = 'TIMEOUT'
        tr.note(f'TIMEOUT: {e}')
    except (OSError, ValueError) as e:
        code = 'UNRESOLVED'
        tr.note(f'UNRESOLVED: {type(e).__name__}: {e}')
    why = (xfail_reason(test, ctx.cut.name) or ctx.cut.xfail.get(test.id)) if code in ('PASS', 'FAIL') else None
    if code == 'CRASH':
        code = 'FAIL'
    if why:
        code = 'XFAIL' if code == 'FAIL' else 'XPASS'
        tr.note(f'{"expected to fail" if code == "XFAIL" else "XPASS: passed but marked XFAIL"}: {why}')
    return code, tr.output(), tr.steps
