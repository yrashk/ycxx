"""Replay a final-override edit and dispatch through peer-owned primary tables."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from linux_compile import run


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


def replay_override(target, scope, cold, resident):
    fixture = Path(__file__).resolve().parents[2] / 'own/classes/virtual-member-final-hidden-override.reject.cpp'
    closed = fixture.read_bytes()
    signature = b'virtual int score(int) const final;'
    assert closed.count(signature) == 1
    open_to_override = closed.replace(signature, b'virtual int score(int) const;')
    line, = [number for number, source in enumerate(closed.splitlines(), 1) if b'//! error' in source]
    source = scope / 'override.cpp'
    syntax = [*target, '-fsyntax-only', source.name]
    source.write_bytes(open_to_override)
    for env in (cold, resident):
        run(syntax, scope, env)

    def reject(env):
        result = subprocess.run(syntax, cwd=scope, env=env, capture_output=True, text=True, timeout=30)
        errors = [message for message in result.stderr.splitlines() if ': error:' in message]
        assert result.returncode != 0 and len(errors) == 1, result.stderr
        assert re.search(rf'override\.cpp:{line}:\d+: error:', errors[0]), result.stderr
        assert "cannot override a virtual function marked 'final'" in errors[0], result.stderr
        return errors[0]

    source.write_bytes(closed)
    assert reject(resident) == reject(cold)
    source.write_bytes(open_to_override)
    for env in (resident, cold):
        run(syntax, scope, env)


def main():
    ycxx, host = sys.argv[1:]
    fixture = Path(__file__).resolve().with_suffix('')
    target = [ycxx, 'cxx', '--target=x86_64-pc-linux-gnu']
    with tempfile.TemporaryDirectory(prefix='ycxx-member-final-') as temp, \
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
        try:
            replay_override(target, scope, cold, resident)
            run([*target, '-c', 'consumer.cpp', '-o', 'cold.o'], scope, cold)
            run([*target, '-c', 'consumer.cpp', '-o', 'consumer.o'], scope, resident)
            assert (scope / 'consumer.o').read_bytes() == (scope / 'cold.o').read_bytes()
            for compiler in native_compilers(host, scope, cold):
                run([compiler, '-std=c++20', '-pedantic-errors', '-O0', '-fno-lto', '-fno-pie',
                     '-c', 'peer.cpp', '-o', 'peer.o'], scope, cold)
                run([compiler, '-no-pie', 'consumer.o', 'peer.o', '-o', 'mixed'], scope, cold)
                run(['./mixed'], scope, cold)
        finally:
            run([ycxx, 'ctl', 'shutdown'], scope, env)


if __name__ == '__main__':
    main()
