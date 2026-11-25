"""Closures that declarations own, across objects and on Linux x86-64 (#338).

On Linux every run test in tests/own/lambda-declarations is compiled by ycxx, linked by the host
compiler and run (their Darwin runs are in the core suite).

On both targets, two units that include the same class, class template and inline variable call
the closures these own: their members are of vague linkage (docs/design/closures.md, decision 4).
Those of an unnamed
namespace's classes and arguments are each unit's own. A member that only constant expressions call has no code. A variable template's specialization, a
closure object that is a class template's member and a closure's static local are strong yet (#124).
"""

import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run

HEADER = '''struct A {
  static constexpr auto f = [](int x) { return x + 1; };
  static constexpr auto g = [](auto x) { return x + 2; };
  static constexpr int folded = [] { return 5; }();
};
template <class T> struct B { static constexpr T m = [](T x) { return x * 2; }(T(3)); };
inline constexpr auto w = [](int x) { return x + 7; };
namespace {
struct Local {
  static constexpr auto f = [](int x) { return x + UNIT; };
  static constexpr auto g = [](auto x) { return x + UNIT; };
};
struct Tag {};
}
template <class T> struct Of { static constexpr auto f = [](int x) { return x + UNIT; }; };
template <class F> int take(F f, int x) { return f(x); }
static int local_sum(int x) { return Local::f(x) + Local::g(x) + take(Local::f, x) + take(Of<Tag>::f, x); }
inline int call(int x) { return A::f(x) + A::g(x) + B<int>::m + w(x) + A::folded; }
'''
UNIT_A = '#define UNIT 100\n#include "h.h"\nint a_local(int x) { return local_sum(x); }\nint a(int x) { return call(x) + A::f(x) + w(x); }\n'
UNIT_B = '#define UNIT 200\n#include "h.h"\nint a(int);\nint a_local(int);\nint main() { int x = 1; if (a_local(x) != 404 || local_sum(x) != 804) return 2; return a(x) == 34 && call(x) == 24 && A::g(x) == 3 && w(x) == 8 ? 0 : 1; }\n'


def main():
    ycxx, host_cxx, os_name = sys.argv[1:]
    linux = os_name == 'linux'
    tests = Path(__file__).resolve().parents[2] / 'own' / 'lambda-declarations'
    env = {key: value for key, value in os.environ.items() if key not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='1')
    if linux:
        env['SDKROOT'] = ''
    target = ['--target=x86_64-pc-linux-gnu'] if linux else []

    with tempfile.TemporaryDirectory(prefix='ycxx-lambda-declarations-') as temp:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        (scope / 'h.h').write_text(HEADER)
        (scope / 'a.cpp').write_text(UNIT_A)
        (scope / 'b.cpp').write_text(UNIT_B)
        for unit in ('a', 'b'):
            run([ycxx, 'cxx', *target, '-std=c++26', '-c', f'{unit}.cpp', '-o', f'{unit}.o'], scope, env)
            symbols = run(['nm', f'{unit}.o'], scope, env)
            for name in ('_ZNK1A1fMUliE_clEi', '_ZNK1A1gMUlT_E_clIiEEDaS1_', '_ZNK1wMUliE_clEi'):
                assert name in symbols, (name, symbols)
            for name in ('6foldedMUl', '1BIiE1mMUl', '__invoke'):
                assert name not in symbols, (name, symbols)
        for order in (('a.o', 'b.o'), ('b.o', 'a.o')):
            if linux:
                run([host_cxx, '-fno-lto', *order, '-o', 'two-units'], scope, env)
            else:
                run([ycxx, 'cxx', *order, '-o', 'two-units'], scope, env)
            run(['./two-units'], scope, env)
        if not linux:
            return

        sources = [s for s in sorted(tests.glob('*.run.cpp')) if '// PEER:' not in s.read_text()]
        assert sources, tests
        for source in sources:
            name = source.name.removesuffix('.run.cpp')
            run([ycxx, 'cxx', *target, '-std=c++26', '-c', source, '-o', f'{name}.o'], scope, env)
            run([host_cxx, '-fno-lto', f'{name}.o', '-o', name], scope, env)
            run([f'./{name}'], scope, env)


if __name__ == '__main__':
    main()
