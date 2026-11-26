"""Defaulted special members across objects and on Linux x86-64 (#157).

Two units that include the same classes with defaulted members, and use all of them, link together:
the definitions are vague linkage ([dcl.fct.def.default], [basic.def.odr]/15). No object defines a
symbol twice. On Linux every run test in tests/own/classes/defaulted-special-members that ycxx
supports there is also compiled by ycxx, linked by the host compiler and run (their Darwin runs are
in the core suite).
"""

from collections import Counter
import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run

HEADER = '''extern int ends;
struct N { int v; N(int x = 1) noexcept : v(x) {} N(const N& o) noexcept : v(o.v) {}
  N& operator=(const N& o) noexcept { v = o.v + 1; return *this; } ~N() { ++ends; } };
struct A { N n; A() noexcept = default; A(const A&) noexcept = default; A& operator=(const A&) noexcept = default; ~A() = default; };
struct P { N n; ~P() = default; virtual int f() { return n.v; } };
struct R { virtual int f() { return 1; } };
template <class T> struct Q : R { T n; Q() noexcept = default; Q(const Q&) noexcept = default; ~Q() = default; };
struct H { A a; Q<N> q; ~H() = default; };
struct I { A a; N n; };
inline int use() noexcept {
  ends = 0;
  { A a; A b(a); b = a; P p; Q<N> q; Q<N> r(q); H h; I i; I j(i); }
  return ends;
}
'''
UNIT_A = '#include "members.h"\nint a() { return use(); }\n'
UNIT_B = '#include "members.h"\nint ends = 0;\nint a();\nint main() { return a() == 11 && use() == 11 ? 0 : 1; }\n'
# Refused on x86-64 Linux: a potentially throwing call with a cleanup, and nested-record parameters.
UNSUPPORTED = {'template-virtual', 'volatile-class'}


def definitions_once(nm, obj, scope, env):
    names = Counter()
    for line in run([nm, obj], scope, env).splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1] not in 'Uuw':
            names[fields[2]] += 1
    twice = [name for name, count in names.items() if count > 1 and not name.startswith(('l_', '.L', 'L'))]
    assert names and not twice, (obj, twice)


def main():
    ycxx, host_cxx, nm, os_name = sys.argv[1:]
    linux = os_name == 'linux'
    tests = Path(__file__).resolve().parents[2] / 'own' / 'classes' / 'defaulted-special-members'
    env = {key: value for key, value in os.environ.items() if key not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='1')
    if linux:
        env['SDKROOT'] = ''
    target = ['--target=x86_64-pc-linux-gnu'] if linux else []

    with tempfile.TemporaryDirectory(prefix='ycxx-defaulted-members-') as temp:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        (scope / 'members.h').write_text(HEADER)
        (scope / 'a.cpp').write_text(UNIT_A)
        (scope / 'b.cpp').write_text(UNIT_B)
        for unit in ('a', 'b'):
            run([ycxx, 'cxx', *target, '-std=c++26', '-c', f'{unit}.cpp', '-o', f'{unit}.o'], scope, env)
            definitions_once(nm, f'{unit}.o', scope, env)
        if linux:
            run([host_cxx, '-fno-lto', 'a.o', 'b.o', '-o', 'two-units'], scope, env)
        else:
            run([ycxx, 'cxx', 'a.o', 'b.o', '-o', 'two-units'], scope, env)
        run(['./two-units'], scope, env)
        if not linux:
            return

        ran = 0
        for source in sorted(tests.glob('*.run.cpp')):
            name = source.name.removesuffix('.run.cpp')
            text = source.read_text()
            if name in UNSUPPORTED or '// XFAIL: ycxx' in text or '// PEER:' in text:
                continue
            run([ycxx, 'cxx', *target, '-std=c++26', '-c', source, '-o', f'{name}.o'], scope, env)
            definitions_once(nm, f'{name}.o', scope, env)
            run([host_cxx, '-fno-lto', f'{name}.o', '-o', name], scope, env)
            run([f'./{name}'], scope, env)
            ran += 1
        assert ran >= 9, ran


if __name__ == '__main__':
    main()
