"""Resolve calls and saved addresses to one external inline function on x86-64 Linux."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    ycxx, host_cxx, readelf, ar = sys.argv[1:]
    fixture = Path(__file__).resolve().parent / 'inline-odr'
    target = '--target=x86_64-pc-linux-gnu'
    env = dict(os.environ, LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='1')
    with tempfile.TemporaryDirectory(prefix='ycxx-inline-', dir='.') as temp:
        def run(*args, status=0, error=''):
            result = subprocess.run(args, cwd=temp, env=env, capture_output=True, text=True, timeout=30)
            if result.returncode != status or error not in result.stderr:
                raise AssertionError(f'{args} exited {result.returncode}, expected {status}'
                                     f' and stderr containing {error!r}\n{result.stdout}{result.stderr}')
            return result.stdout

        def compile(source, obj):
            run(ycxx, 'cxx', target, '-c', source, '-o', obj)

        def link(*inputs, status=0, error=''):
            run(ycxx, 'cxx', target, '-nostdlib', *inputs, '-o', 'program', status=status, error=error)

        inline_symbols = ('_Z12inline_firsti', '_Z12inline_lateri', '_ZN9Operation6memberEi',
                          '_ZN14DeclaredMember6memberEi', '_Z15implicit_inlinei')
        for name in ('main', 'peer'):
            obj = f'{name}.o'
            compile(fixture / f'{name}.cpp', obj)
            table = {row[-1]: row for line in run(readelf, '-Ws', obj).splitlines()
                     if len(row := line.split()) == 8 and row[0].endswith(':')}
            for symbol in inline_symbols:
                assert table[symbol][4] == 'WEAK' and table[symbol][6] != 'UND', table[symbol]
            ordinary = table[f'_Z17{name}_direct_callsi']
            assert ordinary[4] == 'GLOBAL' and ordinary[6] != 'UND', ordinary
            relocations = run(readelf, '-Wr', obj).splitlines()
            for symbol in inline_symbols:
                if symbol == '_ZN14DeclaredMember6memberEi':
                    continue
                for kind in ('R_X86_64_64', 'R_X86_64_PLT32'):
                    assert any(kind in line.split() and symbol in line.split() for line in relocations), (symbol, kind)

        for objects in (('main.o', 'peer.o'), ('peer.o', 'main.o')):
            link(*objects)
            run('./program', 'one', 'two')
        run(ar, 'crs', 'libinline.a', 'peer.o')
        link('main.o', 'libinline.a')
        run('./program', 'one', 'two')

        # The compiler configured by CMake supplies a second producer and drives the system link.
        run(host_cxx, '-std=c++20', '-O0', '-fno-pie', '-c', fixture / 'peer.cpp', '-o', 'host-peer.o')
        for objects in (('main.o', 'host-peer.o'), ('host-peer.o', 'main.o')):
            run(host_cxx, '-no-pie', *objects, '-o', 'host-program')
            run('./host-program', 'one', 'two')

        for name in ('strong1', 'strong2'):
            source = f'{name}.cpp'
            text = 'int ordinary(int value) { return value; }\n'
            if name == 'strong1':
                text += 'int main() { return ordinary(0); }\n'
            (Path(temp) / source).write_text(text)
            compile(source, f'{name}.o')
        link('strong1.o', 'strong2.o', status=1, error="duplicate symbol '_Z8ordinaryi'")


if __name__ == '__main__':
    main()
