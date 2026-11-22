"""Construct an automatic derived object with a native peer's primary vtable."""

import os
from pathlib import Path
import shutil
import sys
import tempfile

from linux_compile import run


def check_maker(readelf, scope, env, obj):
    metadata = run([readelf, '-Wsr', obj], scope, env)
    rows = [line.split() for line in metadata.splitlines()]
    symbols = [row for row in rows if len(row) >= 8 and row[0][:-1].isdigit() and row[0].endswith(':')]
    tables = [row for row in symbols if row[7] == '_ZTV1D']
    assert len(tables) == 1 and tables[0][4:7] == ['GLOBAL', 'DEFAULT', 'UND'], metadata
    assert not any(row[7] == '_ZTV1B' for row in symbols), metadata
    rtti = ('_ZTI1B', '_ZTI1D', '_ZTS1B', '_ZTS1D')
    assert not any(row[7] in rtti and row[6] != 'UND' for row in symbols), metadata
    index = int(tables[0][0][:-1])
    assert any(len(row) >= 5 and row[2].startswith('R_X86_64_') and row[4] == '_ZTV1D'
               and int(row[1], 16) >> 32 == index for row in rows), metadata


def native_compilers(host, scope, env):
    compilers = [host]
    paths = {Path(shutil.which(host) or host).resolve()}
    for variable, default in (('YCXX_GXX', 'g++'), ('YCXX_CLANGXX', 'clang++')):
        compiler = shutil.which(env.get(variable, default))
        if compiler and Path(compiler).resolve() not in paths:
            target = run([compiler, '-dumpmachine'], scope, env).strip()
            if target.startswith('x86_64') and '-linux-' in target:
                compilers.append(compiler)
                paths.add(Path(compiler).resolve())
    return compilers


def main():
    ycxx, host, readelf = sys.argv[1:]
    fixture = Path(__file__).resolve().with_suffix('')
    env = {key: value for key, value in os.environ.items() if key not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', YCXX_NO_DAEMON='1')
    with tempfile.TemporaryDirectory(prefix='ycxx-peer-derived-') as temp:
        scope = Path(temp)
        run([ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-c', fixture / 'maker.cpp', '-o', 'maker.o'], scope, env)
        check_maker(readelf, scope, env, 'maker.o')
        for compiler in native_compilers(host, scope, env):
            flags = [compiler, '-std=c++20', '-pedantic-errors', '-O0', '-fno-lto', '-fno-pie']
            for part in ('peer', 'dispatch'):
                run([*flags, '-c', fixture / f'{part}.cpp', '-o', f'{part}.o'], scope, env)
            run([compiler, '-no-pie', 'maker.o', 'peer.o', 'dispatch.o', '-o', 'mixed'], scope, env)
            run(['./mixed'], scope, env)


if __name__ == '__main__':
    main()
