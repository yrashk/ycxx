"""Run class new- and delete-expressions on Linux x86-64 (#293).

The own run tests in tests/own/class-allocation are compiled by ycxx for Linux, linked by the host
compiler with its own build of their replacement allocation functions, and run. Their Darwin
runs are in the core suite.
"""

import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run


def main():
    ycxx, host_cxx, readelf = sys.argv[1:]
    tests = Path(__file__).resolve().parents[2] / 'own' / 'class-allocation'
    env = {key: value for key, value in os.environ.items() if key not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', SDKROOT='', YCXX_NO_DAEMON='1', YCXX_THREADS='1')

    with tempfile.TemporaryDirectory(prefix='ycxx-class-new-') as temp:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        run([host_cxx, '-std=c++26', '-pedantic-errors', '-O0', '-fno-lto', '-c',
             tests / 'class-allocation-provider.cpp', '-o', 'provider.o'], scope, env)
        # The tests with a PEER line link the replacement allocator; the others the host's.
        for name, peer in (('single-object', True), ('template', True), ('placement', False), ('large', False)):
            obj = f'{name}.o'
            run([ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26', '-c', tests / f'{name}.run.cpp',
                 '-o', obj], scope, env)
            relocations = run([readelf, '--wide', '--relocs', obj], scope, env)
            symbols = {fields[4] for fields in map(str.split, relocations.splitlines())
                       if len(fields) >= 5 and fields[2].startswith('R_X86_64_')}
            # The usual single-object functions: `operator new(size_t)` and the sized delete.
            if name != 'placement':
                assert {'_Znwm', '_ZdlPvm'} <= symbols, relocations
            assert not symbols & {'_ZdlPv', '_Znam', '_ZdaPv'}, relocations
            run([host_cxx, '-fno-lto', obj, *(['provider.o'] if peer else []), '-o', name], scope, env)
            run([f'./{name}'], scope, env)


if __name__ == '__main__':
    main()
