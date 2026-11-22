"""Link native base and complete object calls to separately emitted ELF definitions."""

import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile

from linux_compile import run


# Itanium [mangle.ctor-dtor-name] distinguishes the complete and base entry points.
CONSTRUCTORS = ('_ZN1p4BaseC1Ei', '_ZN1p4BaseC2Ei')
DESTRUCTORS = ('_ZN1p4BaseD1Ev', '_ZN1p4BaseD2Ev')
VARIANTS = frozenset(CONSTRUCTORS + DESTRUCTORS)


def lifecycle_symbols(metadata, variants=VARIANTS):
    rows = (line.split() for line in metadata.splitlines())
    return [row for row in rows if len(row) == 8 and row[0].endswith(':') and row[7] in variants]


def check_provider(readelf, scope, env, obj, *, definitions=True):
    metadata = run([readelf, '-Ws', obj], scope, env)
    symbols = lifecycle_symbols(metadata)
    if not definitions:
        assert not symbols, metadata
        return
    by_name = {row[7]: row for row in symbols}
    assert len(symbols) == 4 and by_name.keys() == VARIANTS, metadata
    for complete, base in (CONSTRUCTORS, DESTRUCTORS):
        entry = by_name[complete]
        assert int(entry[2]) > 0 and entry[3:6] == ['FUNC', 'GLOBAL', 'DEFAULT'], metadata
        assert entry[6].isdigit(), metadata
        # st_value, st_size and st_shndx must describe the same range for both names.
        assert by_name[base][1:7] == entry[1:7], metadata


def check_complete_only_constructor(readelf, scope, env, obj, complete, base):
    metadata = run([readelf, '-Ws', obj], scope, env)
    symbols = lifecycle_symbols(metadata, (complete, base))
    assert [row[7] for row in symbols] == [complete], metadata
    entry = symbols[0]
    assert int(entry[2]) > 0 and entry[3:6] == ['FUNC', 'GLOBAL', 'DEFAULT'] and entry[6].isdigit(), metadata


def check_wide_enum(readelf, scope, env, obj):
    check_complete_only_constructor(readelf, scope, env, obj, '_ZN4BaseC1E5Value', '_ZN4BaseC2E5Value')


def check_peer_calls(readelf, scope, env, obj, expected):
    metadata = run([readelf, '-Wsr', obj], scope, env)
    symbols = lifecycle_symbols(metadata)
    indices = {row[7]: int(row[0][:-1]) for row in symbols}
    assert len(symbols) == 2 and indices.keys() == set(expected), metadata
    assert all(row[6] == 'UND' for row in symbols), metadata
    raw = (scope / obj).read_bytes()
    relocations = []
    section = ''
    offset = 0
    for line in metadata.splitlines():
        header = re.match(r"\s*Relocation section '([^']+)' at offset (0x[\da-f]+)", line)
        if header:
            section = header[1]
            offset = int(header[2], 16)
        row = line.split()
        if len(row) >= 3 and row[2].startswith('R_X86_64_'):
            if len(row) >= 5 and row[4] in VARIANTS:
                assert section == '.rela.text' or section.startswith('.rela.text.'), metadata
                info, = struct.unpack_from('<Q', raw, offset + 8)  # Elf64_Rela::r_info
                assert info >> 32 == indices[row[4]], metadata
                relocations.append(row[4])
            offset += 24  # sizeof(Elf64_Rela)
    assert sorted(relocations) == sorted(expected), metadata


def native_compilers(host, scope, env):
    compilers = [host]
    paths = {Path(shutil.which(host) or host).resolve()}
    for variable, default in (('YCXX_GXX', 'g++'), ('YCXX_CLANGXX', 'clang++')):
        compiler = shutil.which(env.get(variable, default))
        if compiler and Path(compiler).resolve() not in paths:
            target = run([compiler, '-dumpmachine'], scope, env).strip()
            if target.startswith('x86_64') and '-linux-' in target:
                compilers.append(compiler)
                paths.add(Path(compiler).resolve())
    return compilers


def main():
    ycxx, host, readelf = sys.argv[1:]
    fixture = Path(__file__).resolve().with_suffix('')
    invocation = [ycxx, 'cxx', '--target=x86_64-pc-linux-gnu']
    with tempfile.TemporaryDirectory(prefix='ycxx-base-variants-') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        for source in ('base.hpp', 'provider.cpp'):
            shutil.copyfile(fixture / source, scope / source)
        env = {key: value for key, value in os.environ.items() if key not in
               ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT')}
        env.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')
        cold = dict(env, YCXX_NO_DAEMON='1')
        daemon = dict(env, YCXX_DAEMON='require')
        provider = scope / 'provider.cpp'
        original = provider.read_bytes()

        def build_provider(*, definitions=True):
            run([*invocation, '-c', 'provider.cpp', '-o', 'cold.o'], scope, cold)
            run([*invocation, '-c', 'provider.cpp', '-o', 'provider.o'], scope, daemon)
            warm = (scope / 'provider.o').read_bytes()
            assert warm == (scope / 'cold.o').read_bytes()
            check_provider(readelf, scope, cold, 'provider.o', definitions=definitions)
            return warm

        try:
            baseline = build_provider()
            provider.write_bytes(original.split(b'\n\n', 1)[0] + b'\n')
            assert build_provider(definitions=False) != baseline
            provider.write_bytes(original)
            assert build_provider() == baseline

            run([*invocation, '-c', fixture / 'wide_enum.cpp', '-o', 'wide-enum.o'], scope, cold)
            check_wide_enum(readelf, scope, cold, 'wide-enum.o')
            run([*invocation, '-c', fixture / 'callback.cpp', '-o', 'callback.o'], scope, cold)
            check_complete_only_constructor(readelf, scope, cold, 'callback.o',
                                            '_ZN8CallbackC1EPFivE', '_ZN8CallbackC2EPFivE')

            for compiler in native_compilers(host, scope, cold):
                flags = [compiler, '-std=c++20', '-pedantic-errors', '-O0', '-fno-lto', '-fno-pie',
                         '-fno-exceptions', '-fno-rtti']
                for unit in ('derived', 'direct', 'trace'):
                    run([*flags, '-c', fixture / f'{unit}.cpp', '-o', f'{unit}.o'], scope, cold)
                check_peer_calls(readelf, scope, cold, 'derived.o', (CONSTRUCTORS[1], DESTRUCTORS[1]))
                check_peer_calls(readelf, scope, cold, 'direct.o', (CONSTRUCTORS[0], DESTRUCTORS[0]))
                run([compiler, '-no-pie', 'provider.o', 'derived.o', 'direct.o', 'trace.o', '-o', 'mixed'], scope, cold)
                run(['./mixed'], scope, cold)
        finally:
            run([ycxx, 'ctl', 'shutdown'], scope, env)


if __name__ == '__main__':
    main()
