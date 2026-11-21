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

        root = 'struct B { virtual int read(int); };\n'
        (scope / 'type-only.cpp').write_text(root + 'B* identity(B* object) { return object; }\n')
        compile(linux, '-c', 'type-only.cpp', '-o', 'type-only.o')
        assert (scope / 'type-only.o').is_file()
        guards = (
            (root + 'int B::read(int n) { return n; }',
             'an out-of-class definition of a virtual member function is not supported yet'),
            ('struct B { virtual int read(int) = 0; };', "'=' is not supported yet"),
            (root + 'void consume(B object) {}', 'object layout requiring virtual function support'),
            (root + 'constexpr auto size = sizeof(B);', 'object layout requiring virtual function support'),
            (root + 'struct Enclosing { B object; };',
             'native output for a translation unit with virtual member functions is not supported yet'),
        )
        for source, diagnostic in guards:
            (scope / 'unsupported.cpp').write_text(source + '\n')
            compile(linux, '-c', 'unsupported.cpp', '-o', 'unsupported.o', error=diagnostic)
            assert not (scope / 'unsupported.o').exists()

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

            # Run the spec fixture's first-slot variant at the same pointer call site.
            consumer = (scope / 'consumer.cpp').read_text()
            (scope / 'first-slot.cpp').write_text(consumer.replace('object->alpha(argument)', 'object->zeta(argument)', 1))
            host = (scope / 'host.cpp').read_text()
            (scope / 'first-slot-host.cpp').write_text(host.replace('!= 207', '!= 107').replace('!= 413', '!= 313'))
            compile(linux, '-c', 'first-slot.cpp', '-o', 'first-slot.o')
            run([host_cxx, '-std=c++26', '-pedantic-errors', '-no-pie', 'first-slot-host.cpp', 'provider.o',
                 'first-slot.o', '-o', 'mixed-first-slot'], scope, cold)
            run(['./mixed-first-slot'], scope, cold)
        finally:
            run([ycxx, 'ctl', 'shutdown'], scope, base)


if __name__ == '__main__':
    main()
