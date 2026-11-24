"""Run the nested-class run tests on Linux x86-64 (#306).

Every run test in tests/own/classes/nested is compiled by ycxx for Linux, linked by the host
compiler and run. Their Darwin runs are in the core suite. The symbols of a member class's members
are checked to be nested names (Itanium C++ ABI §5.1.5) the host compiler would also use.
"""

import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run


def main():
    ycxx, host_cxx = sys.argv[1:]
    tests = Path(__file__).resolve().parents[2] / 'own' / 'classes' / 'nested'
    env = {key: value for key, value in os.environ.items() if key not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', SDKROOT='', YCXX_NO_DAEMON='1', YCXX_THREADS='1')
    sources = sorted(tests.glob('*.run.cpp'))
    assert sources, tests

    with tempfile.TemporaryDirectory(prefix='ycxx-nested-classes-') as temp:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        for source in sources:
            name = source.name.removesuffix('.run.cpp')
            run([ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26', '-c', source, '-o', f'{name}.o'], scope, env)
            run([host_cxx, '-fno-lto', f'{name}.o', '-o', name], scope, env)
            run([f'./{name}'], scope, env)
        # A member function of a member class, defined out of class, is `O::I::f() const`.
        symbols = run(['nm', 'out-of-class-members.o'], scope, env)
        assert '_ZNK1O1I1fEv' in symbols, symbols
        assert '_ZN1O1I1gE' in symbols, symbols


if __name__ == '__main__':
    main()
