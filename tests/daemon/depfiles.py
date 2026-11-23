"""Check object-producing dependency files and the Ninja consumer (#1016)."""

import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

from process import Env, failures
from system_includes import LINUX


TARGET = (LINUX, '-std=c++26')
SOURCE = 'input #$.cpp'
TOP = 'top #$.hpp'
LEAF = 'leaf #$.hpp'
ALTERNATE = 'alternate #$.hpp'
OBJECT = Path('build/object #$.part.o')
LITERAL_TARGET = r'literal\ target\#$$'
MAIN = '''\
#line 40 "presumed.cpp"
#include "top #$.hpp"
#include "empty.hpp"
#include "macro.hpp"
#include <sys #$.hpp>
#if !__has_include("probe-only.hpp") || __has_include("missing.hpp")
#error wrong include lookup
#endif
int dep_value() { return LEAF + MACRO + SYS; }
'''


def make_inputs(scope):
    (scope / 'system').mkdir()
    (scope / 'build').mkdir()
    for name, text in (
        (SOURCE, MAIN), (LEAF, '#define LEAF 11\n'), (ALTERNATE, '#define LEAF 11\n'),
        ('empty.hpp', ''), ('macro.hpp', '#define MACRO 13\n'), ('probe-only.hpp', ''),
        ('system/sys #$.hpp', '#define SYS 17\n'),
    ):
        (scope / name).write_text(text)


def select_leaf(scope, leaf):
    (scope / TOP).write_text(f'#line 70 "presumed.hpp"\n#include "{leaf}"\n')


def make_quote(path):
    return str(path).replace('$', '$$').replace('#', r'\#').replace(' ', r'\ ')


def check_depfile(scope, data, leaf, target=None, extra=()):
    lhs, colon, rhs = data.decode().replace('\\\n', ' ').partition(':')
    assert colon, data
    if target is None:
        assert lhs in (make_quote(OBJECT), make_quote(scope / OBJECT)), data
    else:
        assert lhs == target, data

    actual = set()
    for token in re.findall(r'(?:\\.|[^\s\\])+', rhs):
        name = token.replace(r'\ ', ' ').replace(r'\#', '#').replace('$$', '$')
        assert make_quote(name) == token, (token, data)
        actual.add((scope / name).resolve())
    expected = (SOURCE, TOP, leaf, 'empty.hpp', 'macro.hpp', 'system/sys #$.hpp', *extra)
    assert actual == {(scope / name).resolve() for name in expected}, (expected, data)


def direct(world, scope):
    make_inputs(scope)
    default = OBJECT.with_suffix('.d')
    args = [*TARGET, '-isystem', 'system', '-c', '-MD', '-o', str(OBJECT), SOURCE]

    def build(context, depfile, *flags):
        (scope / depfile).unlink(missing_ok=True)
        command = [*args, *flags]
        result = world.cxx(command, scope, context, timeout=30)
        assert result == (0, b'', b''), (command, result)
        return (scope / OBJECT).read_bytes(), (scope / depfile).read_bytes()

    snapshots = []
    for leaf in (LEAF, ALTERNATE, LEAF):
        select_leaf(scope, leaf)
        cold = build(world.cold, default)
        warm = build(world.daemon, default)
        assert cold == warm and warm[0].startswith(b'\x7fELF'), leaf
        check_depfile(scope, warm[1], leaf)
        if not snapshots:
            pid = world.pid()
        snapshots.append(warm)
    assert snapshots[0] == snapshots[2] and snapshots[0][1] != snapshots[1][1]
    (scope / default).unlink()
    assert not (scope / (str(OBJECT) + '.d')).exists()

    first, updated = Path('build/first deps.d'), Path('build/updated #$ deps.d')
    reports = []
    for depfile in (first, updated, updated):
        image, report = build(world.daemon, depfile, '-MF', str(depfile), '-MT', LITERAL_TARGET)
        assert image == snapshots[0][0]
        assert not (scope / default).exists()
        check_depfile(scope, report, LEAF, target=LITERAL_TARGET)
        reports.append(report)
    assert reports[0] == reports[1] == reports[2]

    dashed = Path('build/dashed-target.d')
    for env in (world.cold, world.daemon):
        image, report = build(env, dashed, '-MF', str(dashed), '-MT', '-v')
        assert image == snapshots[0][0]
        check_depfile(scope, report, LEAF, target='-v')

    previous = (scope / updated).stat().st_mtime_ns
    (scope / SOURCE).write_text(MAIN + '#error depfile_failure_control\n')
    command = [*args, '-MF', str(updated), '-MT', LITERAL_TARGET]
    for env in (world.cold, world.daemon):
        result = world.cxx(command, scope, env, timeout=30)
        assert result[0] == 1 and b'depfile_failure_control' in result[2], result
        assert (scope / updated).read_bytes() == reports[-1]
        assert (scope / updated).stat().st_mtime_ns == previous
    return pid


def output_integrity(world, scope):
    scope.mkdir()
    (scope / 'real').mkdir()
    (scope / 'alias').symlink_to('real', target_is_directory=True)
    source = scope / 'integrity.cpp'
    source.write_text('int staged_value() { return 41; }\n')
    obj = scope / 'real/t.o'
    prior_dep = scope / 'real/t.d'
    args = [*TARGET, '-c', source.name, '-o', 'real/t.o', '-MD']
    result = world.cxx(args, scope, world.daemon, timeout=30)
    assert result == (0, b'', b''), result
    original = obj.read_bytes()
    assert original.startswith(b'\x7fELF')
    prior_report = prior_dep.read_bytes()
    assert b'integrity.cpp' in prior_report
    prior_stamp = prior_dep.stat().st_mtime_ns

    source.write_text('int staged_value() { return 43; }\n')

    def reject_output(depfile, env):
        result = world.cxx([*args, '-MF', depfile], scope, env, timeout=30)
        assert result[0] == 1 and b'error:' in result[2], (depfile, result)
        assert prior_dep.read_bytes() == prior_report and prior_dep.stat().st_mtime_ns == prior_stamp

    # Path aliases and a missing staging directory must preserve the prior pair.
    depfiles = ['alias/t.o', 'absent-dir/t.d']
    case_alias = scope / 'real/T.o'
    # PARITY: by design: this alias exists only on a case-insensitive scratch filesystem.
    if case_alias.is_file() and case_alias.samefile(obj):
        depfiles.append('real/T.o')
    for depfile in depfiles:
        for env in (world.cold, world.daemon):
            reject_output(depfile, env)
            assert obj.read_bytes() == original, depfile

    # A regular staged depfile cannot replace this directory, even with both parents present.
    blocked = scope / 'deps/blocked.d'
    blocked.mkdir(parents=True)
    guard = blocked / 'guard'
    guard.write_text('existing depfile entry\n')
    link = blocked / 'prior-depfile'
    link.symlink_to(os.path.relpath(prior_dep, blocked))
    entry = blocked.stat()
    for env in (world.cold, world.daemon):
        if not obj.exists():
            obj.write_bytes(original)
        reject_output('deps/blocked.d', env)
        assert blocked.is_dir() and not blocked.is_symlink() and os.path.samestat(entry, blocked.stat())
        assert guard.read_text() == 'existing depfile entry\n'
        assert link.is_symlink() and link.samefile(prior_dep)
        assert (not obj.exists() and not obj.is_symlink()) or obj.read_bytes() == original


def ninja_rebuild(ninja, compiler, scope, env):
    scope.mkdir()
    (scope / 'ninja.cpp').write_text(f'#include "{TOP}"\nint dep_value() {{ return LEAF; }}\n')
    (scope / TOP).write_text(f'#include "{LEAF}"\n')
    leaf = scope / LEAF
    leaf.write_text('#define LEAF 11\n')
    tool = ' '.join(shlex.quote(arg) for arg in compiler).replace('$', '$$')
    (scope / 'build.ninja').write_text(
        f'rule cc\n  command = {tool} -c -MD -MT $out -MF $out.d -o $out $in\n'
        '  depfile = $out.d\n  deps = gcc\nbuild built.o: cc ninja.cpp\n')

    def invoke(*args):
        result = subprocess.run([ninja, *args], cwd=scope, env=env, capture_output=True,
                                text=True, timeout=30)
        assert result.returncode == 0, (args, result)
        return result.stdout

    def unchanged(obj):
        stamp = obj.stat().st_mtime_ns
        assert 'no work to do' in invoke('built.o')
        assert obj.stat().st_mtime_ns == stamp

    invoke('built.o')
    obj = scope / 'built.o'
    initial = obj.read_bytes()
    deps = invoke('-t', 'deps', 'built.o')
    actual = {(scope / line.strip()).resolve() for line in deps.splitlines() if line.startswith('    ')}
    assert actual == {(scope / name).resolve() for name in ('ninja.cpp', TOP, LEAF)}, deps
    unchanged(obj)

    leaf.write_text('#define LEAF 17\n')
    if leaf.stat().st_mtime_ns <= obj.stat().st_mtime_ns:
        stamp = obj.stat().st_mtime_ns + 1
        os.utime(leaf, ns=(stamp, stamp))
    invoke('built.o')
    assert obj.read_bytes() != initial
    unchanged(obj)


def main():
    ycxx, ninja = sys.argv[1:]
    world = Env(str(Path(ycxx).resolve()))
    scope = world.scope.resolve()
    for env in (world.base, world.cold, world.daemon):
        env.update(LC_ALL='C', YCXX_THREADS='2')
    try:
        pid = direct(world, scope)
        output_integrity(world, scope / "integrity")
        if Path(ninja).is_file():
            ninja_rebuild(ninja, (world.ycxx, 'cxx', *TARGET), scope / 'ninja', world.daemon)
        world.same_daemon('depfile edits and Ninja replay', pid)
    finally:
        world.close()
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
