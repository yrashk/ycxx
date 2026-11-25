"""Pass and return empty records across System V AMD64 objects."""

import os
from pathlib import Path
import shutil
import sys
import tempfile

from linux_compile import run


def main():
    ycxx, host = sys.argv[1:]
    linux = '--target=x86_64-pc-linux-gnu'
    darwin = '--target=arm64-apple-darwin'
    fixtures = Path(__file__).resolve().with_suffix('')
    env = {k: v for k, v in os.environ.items() if k not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='2')
    # The bootstrap lane has GCC alone. Exercise both native oracles when provisioned.
    peers = {os.path.realpath(host): host}
    for setting, fallback in (('YCXX_GXX', 'g++-16'), ('YCXX_CLANGXX', 'clang++-23')):
        if peer := shutil.which(os.environ.get(setting, fallback)):
            peers.setdefault(os.path.realpath(peer), peer)

    with tempfile.TemporaryDirectory(prefix='ycxx-empty-record-') as temp:
        scope = Path(temp)

        def execute(*args):
            return run(args, scope, env)

        def compile(target, *args):
            execute(ycxx, 'cxx', target, '-std=c++26', *args)

        for name in ('empty_record_argument_minimal.accept.cpp', 'empty_record_minimal.accept.cpp'):
            source = fixtures / name
            compile(linux, '-nostdlib', source, '-o', 'minimal')
            execute('./minimal')
            compile(darwin, '-c', source, '-o', 'darwin.o')

        for part in ('caller', 'provider'):
            compile(linux, '-c', fixtures / f'empty_record_{part}.cpp', '-o', f'ycxx-{part}.o')
        compile(linux, '-nostdlib', 'ycxx-caller.o', 'ycxx-provider.o', '-o', 'native')
        execute('./native')

        for peer in peers.values():
            for level in ('-O0', '-O2'):
                for part in ('caller', 'provider'):
                    execute(peer, '-std=c++26', '-pedantic-errors', level, '-fno-pie', '-fno-lto', '-c',
                            fixtures / f'empty_record_{part}.cpp', '-o', f'peer-{part}.o')
                for caller, provider in (('ycxx', 'peer'), ('peer', 'ycxx')):
                    execute(peer, '-no-pie', '-fno-lto', f'{caller}-caller.o', f'{provider}-provider.o', '-o', 'mixed')
                    execute('./mixed')


if __name__ == '__main__':
    main()
