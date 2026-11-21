"""Consume a separately compiled provider's virtual table on System V AMD64."""

import os
from pathlib import Path
import shutil
import sys
import tempfile

from linux_compile import run


def main():
    ycxx, host_cxx = sys.argv[1:]
    fixtures = Path(__file__).with_suffix('')
    linux = '--target=x86_64-pc-linux-gnu'
    darwin = '--target=arm64-apple-darwin'
    with tempfile.TemporaryDirectory(prefix='ycxx-virtual-') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        for part in ('consumer', 'provider', 'host'):
            shutil.copyfile(fixtures / f'{part}.cpp', scope / f'{part}.cpp')
        base = {k: v for k, v in os.environ.items() if k not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT')}
        base.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')
        cold = dict(base, YCXX_NO_DAEMON='1')
        daemon = dict(base, YCXX_DAEMON='require')

        def compile(target, *args, env=cold, error=None):
            return run([ycxx, 'cxx', target, *args], scope, env, error)

        compile(linux, '-fsyntax-only', 'consumer.cpp')
        compile(darwin, '-fsyntax-only', 'consumer.cpp',
                error='a call to a virtual member function is not supported yet')
        compile(darwin, '-c', 'consumer.cpp', '-o', 'darwin.o',
                error='a call to a virtual member function is not supported yet')
        assert not (scope / 'darwin.o').exists()
        for part in ('host', 'provider'):
            run([host_cxx, '-std=c++26', '-pedantic-errors', '-frtti', '-fno-lto',
                 '-c', f'{part}.cpp', '-o', f'{part}.o'], scope, cold)
        try:
            for flags in ([], ['-g']):
                compile(linux, *flags, '-c', 'consumer.cpp', '-o', 'cold.o')
                compile(linux, *flags, '-c', 'consumer.cpp', '-o', 'warm.o', env=daemon)
                assert (scope / 'warm.o').read_bytes() == (scope / 'cold.o').read_bytes()
                run([host_cxx, '-no-pie', 'host.o', 'provider.o', 'cold.o', '-o', 'mixed'], scope, cold)
                run(['./mixed'], scope, cold)
        finally:
            run([ycxx, 'ctl', 'shutdown'], scope, base)


if __name__ == '__main__':
    main()
