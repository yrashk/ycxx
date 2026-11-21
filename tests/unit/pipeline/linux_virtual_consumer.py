"""Use separately compiled polymorphic objects on System V AMD64."""

import os
from pathlib import Path
import shutil
import sys
import tempfile

from linux_compile import run


def main():
    ycxx, host_cxx, readelf = sys.argv[1:]
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
        shutil.copytree(fixtures / 'nonkey', scope / 'nonkey')
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
        virtual_definition_error = 'an out-of-class definition of a virtual member function is not supported yet'
        guards = (
            (root + 'int B::read(int n) { return n; }', virtual_definition_error),
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

        measure = scope / 'nonkey' / 'measure.cpp'
        measure_source = measure.read_bytes()
        late_key = (scope / 'nonkey' / 'controls' / 'key-owner.cpp').read_bytes().split(b'\n\n', 1)[1]
        (scope / 'nonkey' / 'late-key.cpp').write_bytes(measure_source + b'\n' + late_key)
        measure_decl = b'    virtual int measure(int) const;\n'
        extra = measure_source.replace(measure_decl, measure_decl + b'    virtual int extra(int) const;\n', 1)
        (scope / 'nonkey' / 'extra.cpp').write_bytes(extra + b'\nint Gauge::extra(int x) const { return x; }\n')
        definition = b'int Gauge::measure(int x) const {'
        mismatch = measure_source.replace(definition, b'int Gauge::measure(int x) const noexcept {', 1)
        (scope / 'nonkey' / 'exception-mismatch.cpp').write_bytes(mismatch)
        (scope / 'nonkey' / 'noexcept.cpp').write_bytes(
            mismatch.replace(measure_decl, b'    virtual int measure(int) const noexcept;\n', 1))
        compile(linux, '-fsyntax-only', 'nonkey/measure.cpp')
        compile(linux, '-fsyntax-only', 'nonkey/controls/in-class.cpp')
        compile(linux, '-c', 'nonkey/noexcept.cpp', '-o', 'nonkey-noexcept.o')
        assert (scope / 'nonkey-noexcept.o').is_file()
        for target, source, diagnostic in (
            (linux, 'nonkey/controls/key-owner.cpp', virtual_definition_error),
            (linux, 'nonkey/controls/inline.cpp', virtual_definition_error),
            (linux, 'nonkey/controls/in-class.cpp', native_virtual_error),
            (linux, 'nonkey/controls/lifetime.cpp', layout_error),
            (linux, 'nonkey/late-key.cpp', virtual_definition_error),
            (linux, 'nonkey/extra.cpp', native_virtual_error),
            (linux, 'nonkey/exception-mismatch.cpp', "conflicting declaration of 'measure'"),
            (darwin, 'nonkey/measure.cpp', virtual_definition_error),
        ):
            compile(target, '-c', source, '-o', 'nonkey-guard.o', error=diagnostic)
            assert not (scope / 'nonkey-guard.o').exists()
        compile(darwin, '-fsyntax-only', 'nonkey/measure.cpp', error=virtual_definition_error)

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

            compile(linux, '-c', 'nonkey/measure.cpp', '-o', 'nonkey-cold.o')
            compile(linux, '-c', 'nonkey/measure.cpp', '-o', 'nonkey-warm.o', env=daemon)
            nonkey_warm = scope / 'nonkey-warm.o'
            assert nonkey_warm.read_bytes() == (scope / 'nonkey-cold.o').read_bytes()
            output = run([readelf, '--wide', '--symbols', 'nonkey-cold.o'], scope, cold)
            symbols = [line.split() for line in output.splitlines()]
            defined = [fields for fields in symbols if len(fields) == 8 and fields[6].isdigit()]
            methods = [fields for fields in defined if fields[7] == '_ZNK5Gauge7measureEi']
            assert len(methods) == 1 and methods[0][3:5] == ['FUNC', 'GLOBAL'] and int(methods[0][2]) > 0, output
            assert not any(fields[7].startswith(('_ZTV', '_ZTI', '_ZTS')) for fields in defined), output

            # Reordering declarations makes the same definition the key. Restore it in the same session.
            anchor_decl = b'    virtual int anchor(int) const;\n'
            assert measure_source.count(anchor_decl + measure_decl) == 1
            measure.write_bytes(measure_source.replace(anchor_decl + measure_decl, measure_decl + anchor_decl, 1))
            nonkey_warm.unlink()
            compile(linux, '-c', 'nonkey/measure.cpp', '-o', 'nonkey-warm.o', env=daemon,
                    error=virtual_definition_error)
            assert not nonkey_warm.exists()
            measure.write_bytes(measure_source)
            compile(linux, '-c', 'nonkey/measure.cpp', '-o', 'nonkey-warm.o', env=daemon)
            assert nonkey_warm.read_bytes() == (scope / 'nonkey-cold.o').read_bytes()

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

                    for part in ('peer', 'caller'):
                        run([provider, '-std=c++20', '-pedantic-errors', '-frtti', '-fno-lto', level,
                             '-c', f'nonkey/{part}.cpp', '-o', f'nonkey-{part}.o'], scope, cold)
                    run([provider, '-frtti', '-fno-lto', 'nonkey-peer.o', 'nonkey-caller.o',
                         'nonkey-cold.o', '-o', 'mixed-nonkey'], scope, cold)
                    run(['./mixed-nonkey'], scope, cold)

            # Editing the base changes the slot while every derived declaration remains the same.
            marker = b'    virtual int marker(int) const;\n'
            score = b'    virtual int score(int) const;\n'
            for part in ('primary', 'primary.peer'):
                source = scope / f'{part}.cpp'
                original = source.read_bytes()
                assert original.count(marker + score) == 1
                source.write_bytes(original.replace(marker + score, score + marker, 1))
            compile(linux, '-c', 'primary.cpp', '-o', 'primary-edited-cold.o')
            compile(linux, '-c', 'primary.cpp', '-o', 'primary-warm.o', env=daemon)
            edited = (scope / 'primary-warm.o').read_bytes()
            assert edited == (scope / 'primary-edited-cold.o').read_bytes()
            assert edited != (scope / 'primary-cold.o').read_bytes()
            run([host_cxx, '-std=c++20', '-pedantic-errors', '-fno-lto', '-c', 'primary.peer.cpp',
                 '-o', 'primary-edited-peer.o'], scope, cold)
            run([host_cxx, '-fno-lto', 'primary-edited-peer.o', 'primary-warm.o',
                 '-o', 'mixed-primary-edited'], scope, cold)
            observed = run(['./mixed-primary-edited'], scope, cold)
            assert observed == 'primary-base consumer ok\n', observed
        finally:
            run([ycxx, 'ctl', 'shutdown'], scope, base)


if __name__ == '__main__':
    main()
