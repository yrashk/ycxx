"""Verify enum fields in System V AMD64 records across separately built objects."""

import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run
from linux_peer_derived_default_construction import native_compilers


def main():
    ycxx, host = sys.argv[1:]
    fixtures = Path(__file__).resolve().with_suffix('')
    env = {key: value for key, value in os.environ.items() if key not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', YCXX_NO_DAEMON='1')
    with tempfile.TemporaryDirectory(prefix='ycxx-enum-fields-') as temporary:
        scope = Path(temporary)
        command = [ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26']

        for part in ('caller', 'provider'):
            run([*command, '-c', fixtures / f'{part}.cpp', '-o', f'ycxx-{part}.o'], scope, env)
        for attribute in ('packed', 'aligned'):
            source = fixtures / f'{attribute}.cpp'
            run([*command, '-fsyntax-only', source], scope, env)
            run([*command, '-c', source, '-o', 'rejected.o'], scope, env,
                error='unsupported aggregate result layout')
            assert not (scope / 'rejected.o').exists()

        for compiler in native_compilers(host, scope, env):
            flags = [compiler, '-std=c++26', '-pedantic-errors', '-O2', '-fno-lto', '-fno-pie']
            for part in ('caller', 'provider'):
                run([*flags, '-c', fixtures / f'{part}.cpp', '-o', f'native-{part}.o'], scope, env)
            for caller, provider in (('ycxx', 'native'), ('native', 'ycxx')):
                run([compiler, '-fno-lto', '-no-pie', f'{caller}-caller.o', f'{provider}-provider.o',
                     '-o', 'mixed'], scope, env)
                run(['./mixed'], scope, env)


if __name__ == '__main__':
    main()
