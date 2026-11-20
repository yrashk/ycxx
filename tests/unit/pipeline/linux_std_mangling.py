"""Check Itanium St encodings and share libycxx's byte operator across producers."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    ycxx, host_cxx, readelf, libycxx_source = sys.argv[1:]
    fixture = Path(__file__).resolve().parent / 'std-mangling'
    libycxx = Path(libycxx_source)
    env = dict(os.environ, LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='1')
    with tempfile.TemporaryDirectory(prefix='ycxx-std-mangling-', dir='.') as temp:
        def run(*args):
            result = subprocess.run(args, cwd=temp, env=env, capture_output=True, text=True, timeout=30)
            if result.returncode != 0:
                raise AssertionError(f'{args} exited {result.returncode}\n{result.stdout}{result.stderr}')
            return result.stdout

        def symbols(obj):
            return {row[-1]: row for line in run(readelf, '-Ws', obj).splitlines()
                    if len(row := line.split()) == 8 and row[0].endswith(':')}

        compiler = (ycxx, 'cxx', '--target=x86_64-pc-linux-gnu')
        includes = ('-isystem', libycxx / 'include')
        run(*compiler, *includes, '-c', fixture / 'byte.cpp', '-o', 'byte.o')
        host_compile = (host_cxx, '-std=c++26', '-O0', '-fno-pie', *includes, '-c')
        run(*host_compile, fixture / 'peer.cpp', '-o', 'peer.o')
        byte_operator = '_ZStorSt4byteS_'
        for obj in ('byte.o', 'peer.o'):
            symbol = symbols(obj).get(byte_operator)
            assert symbol and symbol[4] == 'WEAK' and symbol[6] != 'UND', (obj, symbol)
            relocations = (line.split() for line in run(readelf, '-Wr', obj).splitlines())
            assert any(byte_operator in row and 'R_X86_64_PLT32' in row for row in relocations), obj
        # libycxx's <cstddef> references the static linkage guard defined by its runtime.
        run(*host_compile, libycxx / 'src/runtime/linkage/static.cpp', '-o', 'linkage.o')
        for objects in (('byte.o', 'peer.o'), ('peer.o', 'byte.o')):
            run(host_cxx, '-no-pie', *objects, 'linkage.o', '-o', 'program')
            run('./program', 'one', 'two', 'three', 'four')

        run(*compiler, '-c', fixture / 'names.cpp', '-o', 'names.o')
        expected = {
            '_ZSt6directSt4ItemS_',
            '_ZSt5state',
            '_ZNSt6detail5stateE',
            '_ZNKSt9Inspector7inspectEv',
            '_ZNSt6detail6nestedEv',
            '_Z6chooseSt4ItemNSt6detail4ItemES_S1_',
            '_Z3useNSt6detail4ItemENS_4KindES0_S1_',
            '_Z8classifyPSt3BoxIiEPS_IlES1_S3_',
            '_ZSt12abi_functionIiET_S0_',
            '_ZN5outer3std6directEv',
        }
        defined = {name for name, row in symbols('names.o').items() if row[6] != 'UND'}
        assert expected <= defined, (sorted(expected - defined), sorted(defined))


if __name__ == '__main__':
    main()
