"""Recover complete objects through a native peer's Itanium vtable address point."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from linux_compile import run
from linux_peer_derived_default_construction import native_compilers


def check_consumer(readelf, scope, env):
    metadata = run([readelf, '-Ws', 'consumer.o'], scope, env)
    columns = (line.split() for line in metadata.splitlines())
    symbols = [row for row in columns if len(row) == 8 and row[0].endswith(':') and row[0][:-1].isdigit()]
    assert not any(row[7].startswith(('_ZTI', '_ZTS', '_ZTV', '__dynamic_cast')) for row in symbols), metadata
    supplier = [row for row in symbols if row[7] == '_Z12take_operandv']
    assert len(supplier) == 1 and supplier[0][4:7] == ['GLOBAL', 'DEFAULT', 'UND'], metadata


def replay(ycxx, scope, env):
    target = [ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-c', 'consumer.cpp']
    cold = dict(env, YCXX_NO_DAEMON='1')
    resident = dict(env, YCXX_DAEMON='require')
    header = scope / 'base.hpp'
    original = header.read_bytes()

    def build():
        run([*target, '-o', 'cold.o'], scope, cold)
        run([*target, '-o', 'consumer.o'], scope, resident)
        result = (scope / 'consumer.o').read_bytes()
        assert result == (scope / 'cold.o').read_bytes()
        return result

    def pid():
        status = run([ycxx, 'ctl', 'status'], scope, env)
        found, = (line for line in status.splitlines() if line.startswith('pid: '))
        return found

    def reject(mode):
        result = subprocess.run([*target, '-o', 'rejected.o'], cwd=scope, env=mode,
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 1 and ': error:' in result.stderr, result
        assert not (scope / 'rejected.o').exists(), result
        return result.stderr

    try:
        baseline = build()
        running = pid()
        polymorphic = b'virtual int identify();'
        assert original.count(polymorphic) == 1
        header.write_bytes(original.replace(polymorphic, b'int identify();'))
        assert reject(resident) == reject(cold)

        declaration = b'struct B {'
        assert original.count(declaration) == 1
        header.write_bytes(original.replace(declaration, b'struct Unrelated;\n\n' + declaration))
        assert build() == baseline
        assert pid() == running
    finally:
        run([ycxx, 'ctl', 'shutdown'], scope, env)


def main():
    ycxx, host, readelf = sys.argv[1:]
    fixtures = Path(__file__).resolve().with_suffix('')
    with tempfile.TemporaryDirectory(prefix='ycxx-cast-void-') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-', dir='/tmp') as runtime:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        for source in ('base.hpp', 'consumer.cpp'):
            shutil.copyfile(fixtures / source, scope / source)
        env = {key: value for key, value in os.environ.items()
               if key not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT')}
        env.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')
        replay(ycxx, scope, env)
        check_consumer(readelf, scope, env)
        for compiler in native_compilers(host, scope, env):
            flags = [compiler, '-std=c++20', '-pedantic-errors', '-O0', '-fno-lto', '-fno-pie',
                     '-fno-exceptions', '-fno-rtti']
            run([*flags, '-c', fixtures / 'peer.cpp', '-o', 'peer.o'], scope, env)
            mixed = f'mixed-{Path(compiler).name}'
            run([compiler, '-no-pie', '-fno-lto', 'consumer.o', 'peer.o', '-o', mixed], scope, env)
            run([f'./{mixed}'], scope, env)


if __name__ == '__main__':
    main()
