"""Exchange System V AMD64 reference-field records and exercise the pinned helper (#988)."""

import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run
from linux_peer_derived_default_construction import native_compilers


def main():
    ycxx, host, libycxx = sys.argv[1:]
    libycxx = Path(libycxx).resolve()
    fixtures = Path(__file__).resolve().with_suffix('')
    env = {key: value for key, value in os.environ.items() if key not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='2')
    with tempfile.TemporaryDirectory(prefix='ycxx-reference-fields-') as temporary:
        scope = Path(temporary)
        for source in fixtures.iterdir():
            (scope / source.name).write_bytes(source.read_bytes())

        def compile(source, output, *flags):
            command = [ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26']
            run([*command, *flags, '-c', source, '-o', output], scope, env)

        for part in ('caller', 'provider'):
            compile(f'{part}.cpp', f'ycxx-{part}.o')
        # The current ycxx feature macros select the pin's no-exceptions branch.
        includes = ('-ffreestanding', '-nostdinc', '-isystem', libycxx / 'include')
        compile('closures.cpp', 'closures.o', *includes, '-I', libycxx / 'src/freestanding/new')
        compile(libycxx / 'src/runtime/linkage/static.cpp', 'linkage.o', *includes)

        for compiler in native_compilers(host, scope, env):
            flags = [compiler, '-std=c++26', '-pedantic-errors', '-O0', '-fno-lto', '-fno-pie', '-fno-exceptions']
            for part in ('caller', 'provider'):
                run([*flags, '-c', f'{part}.cpp', '-o', f'native-{part}.o'], scope, env)
            for caller, provider in (('ycxx', 'native'), ('native', 'ycxx')):
                run([compiler, '-fno-lto', '-no-pie', f'{caller}-caller.o', f'{provider}-provider.o',
                     'closures.o', 'linkage.o', '-o', 'mixed'], scope, env)
                run(['./mixed'], scope, env)


if __name__ == '__main__':
    main()
