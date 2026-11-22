"""Default-construct one Linux polymorphic root with a local or external key function."""

import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile

from linux_compile import run


def providers(host_cxx, scope, env):
    result = [host_cxx]
    paths = {Path(shutil.which(host_cxx) or host_cxx).resolve()}
    for variable, fallback in (('YCXX_GXX', 'g++'), ('YCXX_CLANGXX', 'clang++')):
        candidate = shutil.which(env.get(variable, fallback))
        if candidate and Path(candidate).resolve() not in paths:
            target = run([candidate, '-dumpmachine'], scope, env).strip()
            if target.startswith('x86_64') and '-linux-' in target:
                result.append(candidate)
                paths.add(Path(candidate).resolve())
    return result


def check_table_reference(readelf, scope, env, encoding, *, obj='maker-warm.o', mode='maker'):
    output = run([readelf, '-WSsr', obj], scope, env)
    lines = output.splitlines()
    rows = [line.split() for line in lines]
    table = f'_ZTV{encoding}'
    class_symbols = {f'_ZT{kind}{name}' for kind in ('V', 'I', 'S')
                     for name in ('N6abi3094RootE', 'N6abi3097RenamedE')}
    symbols = [row for row in rows if len(row) == 8 and row[0].endswith(':') and row[7] in class_symbols]
    tables = [row for row in symbols if row[7].startswith('_ZTV')]
    assert len(tables) == 1 and tables[0][7] == table, output
    entry = tables[0]
    if mode == 'maker':
        assert len(symbols) == 1 and entry[6] == 'UND', output
    else:
        assert entry[2:6] == ['32', 'OBJECT', 'GLOBAL', 'DEFAULT'] and entry[6].isdigit(), output

    sections = {}
    references = []
    section = ''
    offset = 0
    for line, row in zip(lines, rows):
        definition = re.match(r'\s*\[\s*(\d+)\]\s+(.*)', line)
        if definition and len(definition[2].split()) >= 9:
            sections[int(definition[1])] = definition[2].split()
        header = re.match(r"\s*Relocation section '([^']+)' at offset (0x[\da-f]+)", line)
        if header:
            section = header[1]
            offset = int(header[2], 16)
        if len(row) >= 3 and row[2].startswith('R_X86_64_'):
            if len(row) >= 5 and row[4] == table:
                references.append((section, row[2], offset))
            offset += 24  # sizeof(Elf64_Rela)
    if mode == 'key':
        assert not references, output
        return
    assert len(references) == 1, output
    section, kind, offset = references[0]
    assert kind in ('R_X86_64_GOTPCREL', 'R_X86_64_GOTPCRELX', 'R_X86_64_REX_GOTPCRELX'), output
    relocation_sections = [fields for fields in sections.values() if fields[:2] == [section, 'RELA']]
    assert len(relocation_sections) == 1, output
    link, target, _ = relocation_sections[0][-3:]
    assert [index for index, fields in sections.items() if fields[1] == 'SYMTAB'] == [int(link)], output
    text = sections[int(target)]
    assert text[1] == 'PROGBITS' and 'X' in text[-4], output
    assert text[0] == '.text' or text[0].startswith('.text.'), output
    info, = struct.unpack_from('<Q', (scope / obj).read_bytes(), offset + 8)  # Elf64_Rela::r_info
    assert info >> 32 == int(entry[0][:-1]), output


def main():
    ycxx, host_cxx, readelf = sys.argv[1:]
    fixtures = Path(__file__).with_suffix('')
    with tempfile.TemporaryDirectory(prefix='ycxx-root-default-') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        inputs = {scope / path.name: path.read_bytes() for path in fixtures.iterdir()}
        for path, contents in inputs.items():
            path.write_bytes(contents)
        base = {key: val for key, val in os.environ.items() if key not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT')}
        base.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')
        cold = dict(base, YCXX_NO_DAEMON='1')
        daemon = dict(base, YCXX_DAEMON='require')
        host_providers = providers(host_cxx, scope, cold)
        maker = scope / 'maker.cpp'
        header = scope / 'root.hpp'

        def compile_source(source, obj, env, error=None):
            return run([ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-c', source, '-o', obj], scope, env, error)

        def run_peers(count, key):
            for provider in host_providers:
                flags = [provider, '-std=c++20', '-pedantic-errors', '-O0', '-frtti', '-fno-lto', '-fno-pie']
                units = ('key', 'other', 'peer') if key == 'key.o' else ('other', 'peer')
                for unit in units:
                    run([*flags, f'-DEXPECT_COUNT={count}', '-c', f'{unit}.cpp', '-o', f'{unit}.o'], scope, cold)
                objects = ['maker-warm.o']
                if key:
                    objects.append(key)
                objects.extend(('other.o', 'peer.o'))
                for order in (objects, reversed(objects)):
                    run([provider, '-no-pie', *order, '-o', 'mixed-root'], scope, cold)
                    run(['./mixed-root'], scope, cold)

        def build(encoding, count, mode='maker'):
            compile_source('maker.cpp', 'maker-cold.o', cold)
            compile_source('maker.cpp', 'maker-warm.o', daemon)
            warm = (scope / 'maker-warm.o').read_bytes()
            assert warm == (scope / 'maker-cold.o').read_bytes()
            check_table_reference(readelf, scope, cold, encoding, mode=mode)
            run_peers(count, 'key.o' if mode == 'maker' else None)
            return warm

        def reject(case, error):
            for mode, env in (('cold', cold), ('warm', daemon)):
                obj = f'{case}-{mode}.o'
                compile_source('maker.cpp', obj, env, error)
                assert not (scope / obj).exists()

        try:
            original = build('N6abi3094RootE', 7)

            assert inputs[maker].count(b'root.count = 7;') == 1
            maker.write_bytes(inputs[maker].replace(b'root.count = 7;', b'root.count = 19;'))
            assert build('N6abi3094RootE', 19) != original

            # Change the class name in each TU while keeping the maker's path and daemon session.
            for path, contents in inputs.items():
                renamed = re.sub(rb'\bRoot\b', b'Renamed', contents)
                path.write_bytes(renamed.replace(b'N6abi3094RootE', b'N6abi3097RenamedE'))
            assert build('N6abi3097RenamedE', 7) != original
            for path, contents in inputs.items():
                path.write_bytes(contents)
            assert build('N6abi3094RootE', 7) == original

            encoding = 'N6abi3094RootE'
            include, key_definition = inputs[scope / 'key.cpp'].split(b'\n\n', 1)
            maker_include, maker_definition = inputs[maker].split(b'\n\n', 1)
            assert include == maker_include == b'#include "root.hpp"'
            key_first = include + b'\n\n' + key_definition + b'\n' + maker_definition
            maker_first = include + b'\n\n' + maker_definition + b'\n' + key_definition

            # Reuse the maker path while the key moves to a peer and back.
            maker.write_bytes(key_first)
            joined = build(encoding, 7, 'joined')
            maker.write_bytes(inputs[maker])
            assert build(encoding, 7) == original and original != joined

            compile_source('key.cpp', 'candidate-key.o', cold)
            check_table_reference(readelf, scope, cold, encoding, obj='candidate-key.o', mode='key')
            run_peers(7, 'candidate-key.o')

            maker.write_bytes(key_first)
            assert build(encoding, 7, 'joined') == joined
            maker.write_bytes(maker_first)
            build(encoding, 7, 'joined')
            maker.write_bytes(inputs[maker])

            layout_error = 'object layout requiring virtual function support is not supported yet'
            assert inputs[header].count(b'int count;') == 1
            header.write_bytes(inputs[header].replace(b'int count;', b'int count = 3;'))
            reject('dmi', layout_error)
            header.write_bytes(inputs[header])

            declaration = b'Root root;'
            assert inputs[maker].count(declaration) == 1
            for case, replacement in (
                ('value-init', b'Root root{};'),
                ('declaration-alignment', b'Root __attribute__((aligned(64))) root;'),
                ('typedef-alignment', b'typedef Root AlignedRoot __attribute__((aligned(64)));\n'
                                      b'    AlignedRoot root;'),
            ):
                maker.write_bytes(inputs[maker].replace(declaration, replacement))
                reject(case, layout_error)

            # [stmt.dcl] forbids a jump that bypasses non-vacuous initialization.
            maker.write_text('#include "root.hpp"\n'
                             'int abi309::make_root() {\n'
                             '    goto done;\n'
                             '    Root root;\n'
                             'done:\n'
                             '    return 0;\n'
                             '}\n')
            reject('jump', 'the jump bypasses the initialization of a variable')
        finally:
            run([ycxx, 'ctl', 'shutdown'], scope, base)


if __name__ == '__main__':
    main()
