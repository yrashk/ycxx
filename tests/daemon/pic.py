"""Check PIC driver identity, ELF relocations and the pinned archive inputs (#983, #1089)."""

import json
from pathlib import Path
import re
import subprocess
import sys

from lsp import Editor, expect
from process import Env, failures
from system_includes import DARWIN, LINUX, NATIVE_LINUX


PIC = ('-fPIC',)
MODES = ((), PIC, ())
GOT = {9, 41, 42}  # GOTPCREL, GOTPCRELX, REX_GOTPCRELX
PEER = ('-std=c++26', '-pedantic-errors', '-fPIC', '-fno-lto')
MARKER = '__ycxx_linkage_static_v1'
THREAD_SELF = 'ycxx_pal_thread_self'


def compile_database(world, scope):
    # Exercise the database before this unit receives explicit driver flags.
    unit = scope / 'editor.cpp'
    text = '''\
#if defined(MARK)
#include <pic_database_probe.h>
#if !defined(__APPLE__) || !defined(__aarch64__) || PIC_DATABASE_HEADER != 7
#error imported target or include directory was lost
#endif
#if __PIC__ != 2 || __pic__ != 2 || defined(__PIE__) || defined(__pie__)
#error arm64 Darwin code is always PIC
#endif
#if MARK == 1
int probe() { return pic_darwin_first(); }
#elif MARK == 2
int probe() { return pic_darwin_second(); }
#endif
#elif __PIC__ == 2 && __pic__ == 2 && !defined(__PIE__) && !defined(__pie__)
int probe() { return pic_database_sentinel(); }
#elif __PIC__ == 1 && __pic__ == 1
int probe() { return pic_small_sentinel(); }
#endif
'''
    unit.write_text(text)
    include = scope / 'pic-database-include'
    include.mkdir()
    (include / 'pic_database_probe.h').write_text('#define PIC_DATABASE_HEADER 7\n')
    includes = ('-I', include.name)
    database = scope / 'compile_commands.json'
    editor = Editor(world, scope)
    cases = (
        ((LINUX,), None),
        ((LINUX, *PIC), 'pic_database_sentinel'),
        ((LINUX,), None),
        ((LINUX, '-fpic'), 'pic_small_sentinel'),
        ((LINUX, *PIC, '-fno-pic'), None),
        ((DARWIN, *PIC, '-DMARK=1', *includes), 'pic_darwin_first'),
        ((*PIC, DARWIN, '-DMARK=2', *includes), 'pic_darwin_second'),
        ((DARWIN, '-fno-pic', '-DMARK=1', *includes), 'pic_darwin_first'),
    )
    for version, (flags, sentinel) in enumerate(cases, 1):
        database.write_text(json.dumps([{
            'directory': str(scope), 'file': unit.name,
            'arguments': ['c++', '-std=c++26', *flags, '-c', unit.name, '-o', 'editor.o'],
        }]))
        if version == 1:
            editor.open(unit, text)
        else:
            editor.change(unit, text, version)
        context = f'pic compile database version {version}'
        expected = []
        if sentinel is not None:
            line, spelling = next((i, row) for i, row in enumerate(text.splitlines()) if sentinel in row)
            expected = [(line, spelling.index(sentinel), 1, f"use of undeclared identifier '{sentinel}'")]
        expect(editor.diagnostics(unit, version, context), expected, context)
    expect(editor.finish(), 0, 'pic LSP shutdown')

    # On arm64 Darwin every PIC option leaves the same, always-PIC preprocessing (#1089).
    args = [DARWIN, '-std=c++26', '-DMARK=1', *includes, '-E', unit.name]
    for env in (world.cold, world.daemon):
        for flags in ((), PIC, ('-fpic',), ('-fno-pic',)):
            status, tokens, err = world.cxx([*flags, *args], scope, env, timeout=30)
            assert status == 0 and b'pic_darwin_first' in tokens and not err, (flags, args, status, tokens, err)


def check_exports(elf):
    def function_relocations(name):
        symbol = elf.symbol(name)
        assert symbol.section and symbol.size > 0, (elf.path, name, symbol)
        assert elf.sections[symbol.section].flags & 4, (elf.path, name)  # SHF_EXECINSTR
        return [(row.kind, row.symbol) for row in elf.relocations.get(symbol.section, ())
                if symbol.value <= row.offset < symbol.value + symbol.size]

    for fn, target in (('pic_data_address', 'pic_external_data'), ('pic_function_address', 'pic_external_call')):
        rows = function_relocations(fn)
        assert len(rows) == 1 and rows[0][0] in GOT and rows[0][1] == target, (elf.path, fn, rows)
    assert function_relocations('pic_call') == [(4, 'pic_external_call')], elf.path  # R_X86_64_PLT32
    local = function_relocations('pic_local_address')
    assert len(local) == 1 and local[0][0] == 2, (elf.path, local)  # R_X86_64_PC32

    hidden = elf.symbol('pic_hidden_data')
    assert hidden.other & 3 == 2 and hidden.section != 0, (elf.path, hidden)
    for cell, target in (('pic_data_cell', 'pic_external_data'), ('pic_function_cell', 'pic_external_call')):
        symbol = elf.symbol(cell)
        assert symbol.section and elf.sections[symbol.section].flags & 3 == 3, (elf.path, cell, symbol)
        rows = [row for row in elf.relocations.get(symbol.section, ()) if row.offset == symbol.value]
        assert [(row.kind, row.symbol, row.addend) for row in rows] == [(1, target, 0)], (elf.path, cell, rows)


def check(world, libycxx, host, linker):
    pipeline = Path(__file__).resolve().parents[1] / 'unit/pipeline'
    sys.path.insert(0, str(pipeline))
    from linux_owned_derived_vtable import Elf

    scope = world.scope.resolve()
    fixtures = Path(__file__).resolve().with_suffix('')
    for name in ('exports.cpp', 'peer.cpp', 'pinned.cpp'):
        (scope / name).write_bytes((fixtures / name).read_bytes())

    def cxx(env, *args):
        command = [LINUX, '-std=c++26', *args]
        result = world.cxx(command, scope, env, timeout=30)
        assert result[0] == 0 and not result[2], (command, result)
        return result[1]

    def run(*args):
        result = subprocess.run(args, cwd=scope, env=world.cold, capture_output=True, timeout=30)
        assert result.returncode == 0, (args, result)

    obj = scope / 'exports.o'
    snapshots = []
    for flags, level in zip(MODES, (b'0', b'2', b'0')):
        args = (*flags, '-E', 'exports.cpp')
        tokens = cxx(world.cold, *args)
        assert cxx(world.daemon, *args) == tokens, args
        assert re.search(rb'\bpic_level\s*\(\s*\)\s*\{\s*return\s+' + level + rb'\s*;', tokens), tokens
        if not snapshots:
            pid = world.pid()
        images = []
        for env in (world.cold, world.daemon):
            obj.unlink(missing_ok=True)
            cxx(env, *flags, '-c', 'exports.cpp', '-o', obj.name)
            images.append(obj.read_bytes())
        assert images[0] == images[1], flags
        snapshots.append(images[0])
    assert snapshots[0] == snapshots[2] and snapshots[0] != snapshots[1]
    pic_obj = scope / 'pic-exports.o'
    pic_obj.write_bytes(snapshots[1])
    check_exports(Elf(pic_obj))

    compile_database(world, scope)
    world.same_daemon('pic mode and compile database replay', pid)

    run(host, *PEER, '-DPIC_PROVIDER', '-c', 'peer.cpp', '-o', 'provider.o')
    run(linker, '-shared', '-z', 'text', '-z', 'defs', '-o', 'libexports.so', pic_obj, 'provider.o')
    for flags in ((), ('-DPREEMPT',)):
        run(host, *PEER, *flags, '-pie', '-rdynamic', 'peer.cpp', './libexports.so', '-o', 'exports-program')
        run('./exports-program')

    pin = ('-ffreestanding', '-nostdinc', *PIC, '-isystem', libycxx / 'include')
    pinned = (
        ('src/runtime/linkage/static.cpp', 'marker.o', MARKER),
        ('src/freestanding/pal/thread_self.cpp', 'thread.o', THREAD_SELF),
    )
    for source, output, name in pinned:
        cxx(world.cold, *pin, '-c', libycxx / source, '-o', output)
        elf = Elf(scope / output)
        symbol = elf.symbol(name)
        assert symbol.other & 3 == 2 and symbol.section != 0, (output, symbol)
    run(linker, '-shared', '-z', 'text', '-z', 'defs', '-o', 'libpinned.so', 'marker.o', 'thread.o')
    run(world.ycxx, 'archive', '-o', 'pinned.a', 'marker.o', 'thread.o')
    run(host, *PEER, '-isystem', libycxx / 'include', '-pie', 'pinned.cpp', 'pinned.a', '-o', 'pinned-program')
    run('./pinned-program')


def main():
    # PARITY: by design: the relocation and shared-link checks use x86-64 Linux ELF.
    if not NATIVE_LINUX:
        return 0
    ycxx, libycxx = (Path(arg).resolve() for arg in sys.argv[1:3])
    world = Env(str(ycxx))
    for env in (world.base, world.cold, world.daemon):
        env.update(LC_ALL='C', YCXX_THREADS='2')
    try:
        check(world, libycxx, *sys.argv[3:])
    finally:
        world.close()
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
