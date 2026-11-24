"""Dispatch pure slots through peer-owned primary and secondary base objects."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from linux_compile import run


def check_consumer_symbols(readelf, scope, env, obj):
    forbidden = {
        '_ZNK10pure_probe4Root4zetaEi', '_ZNK10pure_probe4Root5alphaEi',
        '_ZTVN10pure_probe4RootE', '_ZTIN10pure_probe4RootE', '_ZTSN10pure_probe4RootE',
        '__cxa_pure_virtual',
    }
    metadata = run([readelf, '-Ws', obj], scope, env)
    rows = (line.split() for line in metadata.splitlines())
    names = {row[7].partition('@')[0] for row in rows
             if len(row) >= 8 and row[0].endswith(':') and row[0][:-1].isdigit()}
    assert not names & forbidden, metadata


def check_potentially_throwing_slot(target, scope, cold, resident):
    # Without `noexcept` the root is no longer the borrowed pure-interface profile but an ordinary
    # dynamic class (#309): the consumer still compiles, cold and resident alike, and still emits
    # no table, RTTI or pure-virtual handler for a root it never constructs.
    objects = []
    for mode, env in (('cold', cold), ('resident', resident)):
        obj = f'throwing-{mode}.o'
        result = subprocess.run([*target, '-c', 'consumer.cpp', '-o', obj], cwd=scope, env=env,
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        objects.append((scope / obj).read_bytes())
    assert objects[0] == objects[1]
    return 'throwing-cold.o'


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
    target = [ycxx, 'cxx', '--target=x86_64-pc-linux-gnu']
    with tempfile.TemporaryDirectory(prefix='ycxx-pure-dispatch-') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        for source in ('root.hpp', 'consumer.cpp', 'peer.cpp'):
            shutil.copyfile(fixture / source, scope / source)
        env = {key: value for key, value in os.environ.items() if key not in
               ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT')}
        env.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')
        cold = dict(env, YCXX_NO_DAEMON='1')
        resident = dict(env, YCXX_DAEMON='require')
        peers = native_compilers(host, scope, cold)

        def build_case():
            run([*target, '-c', 'consumer.cpp', '-o', 'cold.o'], scope, cold)
            run([*target, '-c', 'consumer.cpp', '-o', 'consumer.o'], scope, resident)
            payload = (scope / 'consumer.o').read_bytes()
            assert payload == (scope / 'cold.o').read_bytes()
            check_consumer_symbols(readelf, scope, cold, 'consumer.o')
            for compiler in peers:
                run([compiler, '-std=c++20', '-pedantic-errors', '-O0', '-fno-lto', '-fno-pie',
                     '-c', 'peer.cpp', '-o', 'peer.o'], scope, cold)
                run([compiler, '-no-pie', 'consumer.o', 'peer.o', '-o', 'mixed'], scope, cold)
                run(['./mixed'], scope, cold)
            return payload

        try:
            original = build_case()
            header = scope / 'root.hpp'
            source = header.read_bytes()
            zeta = b'    virtual int zeta(int value) const noexcept = 0;\n'
            alpha = b'    virtual int alpha(int value) const noexcept = 0;\n'
            assert source.count(zeta + alpha) == 1
            header.write_bytes(source.replace(zeta, zeta.replace(b' noexcept', b'')))
            check_consumer_symbols(readelf, scope, cold, check_potentially_throwing_slot(target, scope, cold, resident))
            header.write_bytes(source)
            assert build_case() == original
            header.write_bytes(source.replace(zeta + alpha, alpha + zeta))
            assert build_case() != original
        finally:
            run([ycxx, 'ctl', 'shutdown'], scope, env)


if __name__ == '__main__':
    main()
