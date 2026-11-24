"""Nested classes across objects and on Linux x86-64 (#306).

On Linux every run test in tests/own/classes/nested is compiled by ycxx, linked by the host
compiler and run (their Darwin runs are in the core suite), and a member class's out-of-class
members are checked to have nested names (Itanium C++ ABI §5.1.5).

On both targets, two units that include one class definition with a constexpr static data member
and an inline member function of a member class link together: they are vague linkage, as those
of a namespace-scope class are ([basic.def.odr]/15). A non-constexpr `static inline` member is
strong for every class yet (#124).
"""

import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run

HEADER = '''struct O {
  struct I {
    static constexpr int c = 3;
    int get() const { return c + 2; }
  };
};
'''
UNIT_A = '#include "o.h"\nint a() { O::I i; return i.get() + O::I::c; }\n'
UNIT_B = '#include "o.h"\nint a();\nint main() { O::I i; return a() + i.get() + O::I::c - 16; }\n'


def main():
    ycxx, host_cxx, os_name = sys.argv[1:]
    linux = os_name == 'linux'
    tests = Path(__file__).resolve().parents[2] / 'own' / 'classes' / 'nested'
    env = {key: value for key, value in os.environ.items() if key not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='1')
    if linux:
        env['SDKROOT'] = ''
    target = ['--target=x86_64-pc-linux-gnu'] if linux else []

    with tempfile.TemporaryDirectory(prefix='ycxx-nested-classes-') as temp:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        (scope / 'o.h').write_text(HEADER)
        (scope / 'a.cpp').write_text(UNIT_A)
        (scope / 'b.cpp').write_text(UNIT_B)
        for unit in ('a', 'b'):
            run([ycxx, 'cxx', *target, '-std=c++26', '-c', f'{unit}.cpp', '-o', f'{unit}.o'], scope, env)
        if linux:
            run([host_cxx, '-fno-lto', 'a.o', 'b.o', '-o', 'two-units'], scope, env)
        else:
            run([ycxx, 'cxx', 'a.o', 'b.o', '-o', 'two-units'], scope, env)
        run(['./two-units'], scope, env)
        if not linux:
            return

        sources = sorted(tests.glob('*.run.cpp'))
        assert sources, tests
        for source in sources:
            name = source.name.removesuffix('.run.cpp')
            run([ycxx, 'cxx', *target, '-std=c++26', '-c', source, '-o', f'{name}.o'], scope, env)
            run([host_cxx, '-fno-lto', f'{name}.o', '-o', name], scope, env)
            run([f'./{name}'], scope, env)
        # A member function of a member class, defined out of class, is `O::I::f() const`.
        symbols = run(['nm', 'out-of-class-members.o'], scope, env)
        assert '_ZNK1O1I1fEv' in symbols, symbols
        assert '_ZN1O1I1gE' in symbols, symbols


if __name__ == '__main__':
    main()
