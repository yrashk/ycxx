"""Use separately compiled polymorphic objects on System V AMD64."""

import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

from linux_compile import run


def defined_symbols(output):
    rows = [line.split() for line in output.splitlines()]
    return [fields for fields in rows if len(fields) == 8 and fields[6].isdigit()]


def check_owned_root_object(readelf, obj, scope, env, slots):
    metadata = run([readelf, '-WSsrg', obj], scope, env)
    vtable = '_ZTVN6abi3094RootE'
    rtti = '_ZTIN6abi3094RootE'
    name = '_ZTSN6abi3094RootE'
    symbols = {}
    definitions = defined_symbols(metadata)
    for symbol, size, binding in ((vtable, 32, 'GLOBAL'), (rtti, 16, 'GLOBAL'), (name, 15, 'WEAK')):
        matches = [fields for fields in definitions if fields[7] == symbol]
        assert len(matches) == 1, metadata
        entry = matches[0]
        assert entry[2:6] == [str(size), 'OBJECT', binding, 'DEFAULT'], metadata
        symbols[symbol] = int(entry[6]), int(entry[1], 16)

    sections = {}
    for line in metadata.splitlines():
        section = re.match(r'\s*\[\s*(\d+)\]\s+(.*)', line)
        if section:
            fields = section[2].split()
            if len(fields) >= 9 and fields[1] in ('PROGBITS', 'RELA'):
                sections[int(section[1])] = fields
    destinations = {fields[0]: int(fields[-2]) for fields in sections.values() if fields[1] == 'RELA'}
    relocations = {}
    current = None
    for line in metadata.splitlines():
        reloc_section = re.match(r"Relocation section '([^']+)'", line)
        if reloc_section:
            current = destinations[reloc_section[1]]
        fields = line.split()
        if len(fields) == 7 and fields[2].startswith('R_X86_64_'):
            offset, _, kind, _, target, sign, addend = fields
            relocations.setdefault(current, []).append((int(offset, 16), kind, target, sign, addend))

    names = tuple(f'_ZN6abi3094Root{len(slot)}{slot}Ev' for slot in slots)
    for symbol, size, pointers in (
        (vtable, 32, ((8, rtti, '0'), (16, names[0], '0'), (24, names[1], '0'))),
        (rtti, 16, ((0, '_ZTVN10__cxxabiv117__class_type_infoE', '10'), (8, name, '0'))),
        (name, 15, ()),
    ):
        ndx, value = symbols[symbol]
        actual = sorted((offset - value, *fields) for offset, *fields in relocations.get(ndx, ())
                        if value <= offset < value + size)
        expected = [(offset, 'R_X86_64_64', target, '+', addend) for offset, target, addend in pointers]
        assert actual == expected, metadata

    raw = (scope / obj).read_bytes()
    for symbol, expected in ((vtable, b'\0' * 8), (name, b'N6abi3094RootE\0')):
        ndx, value = symbols[symbol]
        start = int(sections[ndx][3], 16) + value
        assert raw[start:start + len(expected)] == expected, metadata
    name_section, _ = symbols[name]
    assert sections[name_section][6] == 'AG', metadata
    assert re.search(rf'\[{name}\] contains 1 sections:\n.*\n\s*\[\s*{name_section}\]', metadata), metadata


def check_owned_root(compile, linux, scope, cold, daemon, host_cxx, readelf):
    parts = ('first', 'second', 'virtual-call')

    def check_stage(slots, expected_first):
        owner, peer = slots
        objects = {}
        for part in parts:
            obj = f'root-{part}'
            source = f'owned-root/{part}.cpp'
            compile(linux, '-c', source, '-o', f'{obj}-cold.o')
            compile(linux, '-c', source, '-o', f'{obj}-warm.o', env=daemon)
            objects[part] = (scope / f'{obj}-warm.o').read_bytes()
            assert objects[part] == (scope / f'{obj}-cold.o').read_bytes(), part
            if part != owner:
                metadata = run([readelf, '-Ws', f'{obj}-warm.o'], scope, cold)
                assert not any(fields[7].startswith(('_ZTV', '_ZTI', '_ZTS'))
                               for fields in defined_symbols(metadata)), metadata
        check_owned_root_object(readelf, f'root-{owner}-warm.o', scope, cold, slots)

        host_flags = [host_cxx, '-std=c++26', '-pedantic-errors', '-frtti', '-fno-lto']
        run([*host_flags, f'-DEXPECT_FIRST={expected_first}', '-c', 'owned-root/host.cpp',
             '-o', 'root-host.o'], scope, cold)
        run([*host_flags, '-c', f'owned-root/{peer}.cpp', '-o', 'root-peer.o'], scope, cold)
        run([host_cxx, '-no-pie', 'root-host.o', f'root-{owner}-warm.o', 'root-peer.o',
             'root-virtual-call-warm.o', '-o', 'mixed-root'], scope, cold)
        run(['./mixed-root'], scope, cold)
        return objects

    original = check_stage(('first', 'second'), 71)

    first = scope / 'owned-root' / 'first.cpp'
    first_source = first.read_bytes()
    assert first_source.count(b'return 71;') == 1
    first.write_bytes(first_source.replace(b'return 71;', b'return 109;'))
    body_only = check_stage(('first', 'second'), 109)
    assert body_only['first'] != original['first']
    assert all(body_only[part] == original[part] for part in ('second', 'virtual-call'))
    first.write_bytes(first_source)

    header = scope / 'owned-root' / 'root.hpp'
    source = header.read_bytes()
    declarations = b'    virtual int first();\n    virtual int second();\n'
    assert source.count(declarations) == 1
    header.write_bytes(source.replace(declarations, b'    virtual int second();\n    virtual int first();\n'))
    reordered = check_stage(('second', 'first'), 71)
    assert all(reordered[part] != original[part] for part in parts)

    header.write_bytes(source)
    for part in parts:
        obj = f'root-{part}-warm.o'
        compile(linux, '-c', f'owned-root/{part}.cpp', '-o', obj, env=daemon)
        assert (scope / obj).read_bytes() == original[part], part


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
        shutil.copytree(fixtures / 'owned-root', scope / 'owned-root')
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
        nonthrowing_decl = b'    virtual int measure(int) const noexcept;\n'
        nonthrowing = mismatch.replace(measure_decl, nonthrowing_decl, 1)
        (scope / 'nonkey' / 'exception-parameter-definition.cpp').write_bytes(
            b'char x;\n' + nonthrowing.replace(b'noexcept {', b'noexcept(sizeof(x) == 1) {', 1))
        conditional_decl = b'    virtual int measure(int x) const noexcept(sizeof(x) == 1);\n'
        (scope / 'nonkey' / 'exception-parameter-declaration.cpp').write_bytes(
            b'char x;\n' + nonthrowing.replace(nonthrowing_decl, conditional_decl, 1))
        head, body = measure_source.split(b'\n\n', 1)
        inline_friend = b'\n\nstruct Friend { friend inline int Gauge::measure(int) const; };\n'
        # [dcl.inline]: this nondefining friend needs an earlier inline declaration.
        (scope / 'nonkey' / 'inline-friend.cpp').write_bytes(head + inline_friend + body)
        (scope / 'nonkey' / 'prior-inline-friend.cpp').write_bytes(
            head.replace(b'virtual int measure', b'virtual inline int measure', 1) + inline_friend)
        compile(linux, '-fsyntax-only', 'nonkey/measure.cpp')
        compile(linux, '-fsyntax-only', 'nonkey/controls/in-class.cpp')
        compile(linux, '-fsyntax-only', 'nonkey/prior-inline-friend.cpp')
        for source in (nonthrowing, nonthrowing.replace(b'noexcept {', b'noexcept(true) {', 1),
                       mismatch.replace(b'noexcept {', b'noexcept(false) {', 1)):
            (scope / 'nonkey' / 'noexcept.cpp').write_bytes(source)
            compile(linux, '-c', 'nonkey/noexcept.cpp', '-o', 'nonkey-noexcept.o')
            assert (scope / 'nonkey-noexcept.o').is_file()
            (scope / 'nonkey-noexcept.o').unlink()
        for target, source, diagnostic in (
            (linux, 'nonkey/controls/inline.cpp', virtual_definition_error),
            (linux, 'nonkey/controls/in-class.cpp', native_virtual_error),
            (linux, 'nonkey/controls/lifetime.cpp', layout_error),
            (linux, 'nonkey/late-key.cpp', native_virtual_error),
            (linux, 'nonkey/extra.cpp', native_virtual_error),
            (linux, 'nonkey/exception-mismatch.cpp', "conflicting declaration of 'measure'"),
            (linux, 'nonkey/exception-parameter-definition.cpp', virtual_definition_error),
            (linux, 'nonkey/exception-parameter-declaration.cpp', virtual_definition_error),
            (linux, 'nonkey/inline-friend.cpp', "'friend' is not supported yet"),
            (darwin, 'nonkey/measure.cpp', virtual_definition_error),
        ):
            compile(target, '-c', source, '-o', 'nonkey-guard.o', error=diagnostic)
            assert not (scope / 'nonkey-guard.o').exists()
        compile(darwin, '-fsyntax-only', 'nonkey/measure.cpp', error=virtual_definition_error)
        compile(linux, '-c', 'nonkey/controls/key-owner.cpp', '-o', 'nonkey-key-owner.o')
        output = run([readelf, '-Ws', 'nonkey-key-owner.o'], scope, cold)
        assert {'_ZTV5Gauge', '_ZTI5Gauge', '_ZTS5Gauge'} <= {fields[7] for fields in defined_symbols(output)}, output

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
            defined = defined_symbols(output)
            methods = [fields for fields in defined if fields[7] == '_ZNK5Gauge7measureEi']
            assert len(methods) == 1 and methods[0][3:5] == ['FUNC', 'GLOBAL'] and int(methods[0][2]) > 0, output
            assert not any(fields[7].startswith(('_ZTV', '_ZTI', '_ZTS')) for fields in defined), output

            # Reordering declarations makes the same definition the key. Restore it in the same session.
            anchor_decl = b'    virtual int anchor(int) const;\n'
            assert measure_source.count(anchor_decl + measure_decl) == 1
            measure.write_bytes(measure_source.replace(anchor_decl + measure_decl, measure_decl + anchor_decl, 1))
            compile(linux, '-c', 'nonkey/measure.cpp', '-o', 'nonkey-edited-cold.o')
            compile(linux, '-c', 'nonkey/measure.cpp', '-o', 'nonkey-warm.o', env=daemon)
            assert nonkey_warm.read_bytes() == (scope / 'nonkey-edited-cold.o').read_bytes()
            output = run([readelf, '-Ws', 'nonkey-warm.o'], scope, cold)
            assert {'_ZTV5Gauge', '_ZTI5Gauge', '_ZTS5Gauge'} <= {f[7] for f in defined_symbols(output)}, output
            measure.write_bytes(measure_source)
            compile(linux, '-c', 'nonkey/measure.cpp', '-o', 'nonkey-warm.o', env=daemon)
            assert nonkey_warm.read_bytes() == (scope / 'nonkey-cold.o').read_bytes()

            check_owned_root(compile, linux, scope, cold, daemon, host_cxx, readelf)
            hidden_namespace = (scope / 'owned-root' / 'prior-namespace.cpp').read_bytes()
            forward_declaration = b'    struct Gauge;\n'
            assert hidden_namespace.count(forward_declaration) == 1
            (scope / 'owned-root' / 'prior-namespace-friend.cpp').write_bytes(hidden_namespace.replace(
                forward_declaration, b'    struct Witness { friend struct Gauge; };\n'))
            for part, diagnostic in (
                ('prior-namespace', virtual_definition_error),
                ('prior-namespace-friend', native_virtual_error),
                ('slot-redeclaration-after', native_virtual_error),
            ):
                for mode, env in (('cold', cold), ('warm', daemon)):
                    obj = f'{part}-{mode}.o'
                    compile(linux, '-c', f'owned-root/{part}.cpp', '-o', obj, env=env, error=diagnostic)
                    assert not (scope / obj).exists()

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
