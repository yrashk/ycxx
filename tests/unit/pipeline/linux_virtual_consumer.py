"""Use separately compiled polymorphic objects on System V AMD64."""

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
        for part in ('consumer', 'provider', 'host', 'layout-consumer', 'layout-provider-host',
                     'primary', 'primary.peer', 'produce', 'const-write'):
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
        layout_error = 'object layout requiring virtual function support'
        (scope / 'opaque-size.cpp').write_text(root + 'constexpr auto size = sizeof(B);\n')
        compile(linux, '-fsyntax-only', 'opaque-size.cpp')
        compile(darwin, '-fsyntax-only', 'opaque-size.cpp', error=layout_error)
        scalar = 'struct B { virtual int read(int); unsigned int word; };\n'
        (scope / 'inherited.cpp').write_text(
            scalar + 'struct D : B {}; unsigned int read(D* object) { return object->word; }\n')
        compile(linux, '-fsyntax-only', 'inherited.cpp')
        compile(linux, '-c', 'inherited.cpp', '-o', 'inherited.o')
        assert (scope / 'inherited.o').is_file()
        compile(darwin, '-fsyntax-only', 'inherited.cpp', error=layout_error)
        compile(darwin, '-c', 'inherited.cpp', '-o', 'inherited-darwin.o', error=layout_error)
        assert not (scope / 'inherited-darwin.o').exists()

        native_virtual_error = 'native output for a translation unit with virtual member functions is not supported yet'
        guards = (
            (root + 'int B::read(int n) { return n; }',
             'an out-of-class definition of a virtual member function is not supported yet'),
            ('struct B { virtual int read(int) = 0; };', "'=' is not supported yet"),
            (root + 'void consume(B object) {}', layout_error),
            (root + 'struct Enclosing { B object; };', native_virtual_error),
            (scalar + 'B object;', layout_error),
            (scalar + 'void copy(B& to, const B& from) { to = from; }', layout_error),
            (scalar + 'void consume(B object) {}', layout_error),
            (scalar + 'B make(); unsigned int read() { return make().word; }', layout_error),
            ('#pragma pack(push, 1)\n' + scalar + '#pragma pack(pop)\nunsigned long size() { return sizeof(B); }',
             layout_error),
            (scalar.replace('struct B', 'struct __attribute__((packed)) B') +
             'unsigned int read(B* object) { return object->word; }', layout_error),
            (scalar.replace('read(int);', 'read(int n) { return n; }'), native_virtual_error),
            (scalar + 'struct D : B { virtual int extra(int); };', native_virtual_error),
            (scalar + 'struct D : B { int read(int n) override { return n; } };', native_virtual_error),
            (scalar + 'struct D : B {}; struct E : D {};', native_virtual_error),
        )
        for source, diagnostic in guards:
            (scope / 'unsupported.cpp').write_text(source + '\n')
            compile(linux, '-c', 'unsupported.cpp', '-o', 'unsupported.o', error=diagnostic)
            assert not (scope / 'unsupported.o').exists()
        for target in (linux, darwin):
            compile(target, '-fsyntax-only', 'produce.cpp', error=layout_error)
            compile(target, '-c', 'produce.cpp', '-o', 'produce.o', error=layout_error)
            assert not (scope / 'produce.o').exists()
        const_error = 'cannot assign to a variable of const-qualified type'
        compile(linux, '-fsyntax-only', 'const-write.cpp', error=const_error)
        compile(linux, '-c', 'const-write.cpp', '-o', 'const-write.o', error=const_error)
        assert not (scope / 'const-write.o').exists()

        for part in ('host', 'provider'):
            run([host_cxx, '-std=c++26', '-pedantic-errors', '-frtti', '-fno-lto',
                 '-c', f'{part}.cpp', '-o', f'{part}.o'], scope, cold)
        try:
            compile(linux, '-c', 'consumer.cpp', '-o', 'cold.o')
            compile(linux, '-c', 'consumer.cpp', '-o', 'warm.o', env=daemon)
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

            compile(linux, '-fsyntax-only', 'layout-consumer.cpp')
            compile(darwin, '-c', 'layout-consumer.cpp', '-o', 'layout-darwin.o', error=layout_error)
            assert not (scope / 'layout-darwin.o').exists()
            compile(linux, '-c', 'layout-consumer.cpp', '-o', 'layout-cold.o')
            compile(linux, '-c', 'layout-consumer.cpp', '-o', 'layout-warm.o', env=daemon)
            assert (scope / 'layout-warm.o').read_bytes() == (scope / 'layout-cold.o').read_bytes()

            compile(linux, '-fsyntax-only', 'primary.cpp')
            compile(darwin, '-fsyntax-only', 'primary.cpp', error=layout_error)
            compile(darwin, '-c', 'primary.cpp', '-o', 'primary-darwin.o', error=layout_error)
            assert not (scope / 'primary-darwin.o').exists()
            compile(linux, '-c', 'primary.cpp', '-o', 'primary-cold.o')
            compile(linux, '-c', 'primary.cpp', '-o', 'primary-warm.o', env=daemon)
            assert (scope / 'primary-warm.o').read_bytes() == (scope / 'primary-cold.o').read_bytes()

            providers = [host_cxx]
            clang = shutil.which(cold.get('YCXX_CLANGXX', 'clang++'), path=cold.get('PATH'))
            if clang and clang != host_cxx:
                target = run([clang, '-dumpmachine'], scope, cold).strip()
                if target.startswith('x86_64') and '-linux-' in target:
                    providers.append(clang)
            for provider in providers:
                for level in ('-O0', '-O2'):
                    run([provider, '-std=c++20', '-pedantic-errors', '-fno-lto', level,
                         '-c', 'layout-provider-host.cpp', '-o', 'layout-provider.o'], scope, cold)
                    run([provider, '-fno-lto', 'layout-provider.o', 'layout-cold.o', '-o', 'mixed-layout'], scope, cold)
                    observed = run(['./mixed-layout'], scope, cold)
                    assert len(observed.splitlines()) == 19, observed

                    run([provider, '-std=c++20', '-pedantic-errors', '-fno-lto', level,
                         '-c', 'primary.peer.cpp', '-o', 'primary-peer.o'], scope, cold)
                    run([host_cxx, '-fno-lto', 'primary-peer.o', 'primary-cold.o', '-o', 'mixed-primary'], scope, cold)
                    observed = run(['./mixed-primary'], scope, cold)
                    assert observed == 'primary-base consumer ok\n', observed
        finally:
            run([ycxx, 'ctl', 'shutdown'], scope, base)


if __name__ == '__main__':
    main()
