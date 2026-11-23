"""Check RTTI mode through the driver, compile database and Linux ELF output (#982)."""

import json
from pathlib import Path
import re
import subprocess
import sys

from lsp import Editor, expect
from process import Env, failures
from system_includes import LINUX, NATIVE_LINUX


ENABLED = ('-frtti',)
DISABLED = ('-fno-rtti',)
MODES = (
    ((), True),
    (DISABLED, False),
    (ENABLED, True),
    ((*ENABLED, *DISABLED), False),
    ((*DISABLED, *ENABLED), True),
    ((), True),
)
DOWNCAST = '''\
struct Base { virtual int step(); };
struct Derived : Base { int step() override; };
Derived* downcast(Base* source) {
  return dynamic_cast<Derived*>(source);
}
'''
TYPEID = 'namespace std { class type_info; }\nvoid inspect() { (void)typeid(int); }\n'
THROW = 'extern "C" void throw_int() { throw 17; }\n'
TABLE = '_ZTV1D'
KEY = '_ZN1D1fEv'
DESCRIPTORS = ('_ZTI1B', '_ZTI1D', '_ZTS1B', '_ZTS1D')


def disabled_diagnostic(message):
    return re.search(r'\brtti\b|-f(?:no-)?rtti\b', message, re.IGNORECASE) and re.search(
        r'disabl|requires?|-f(?:no-)?rtti|not enabled', message, re.IGNORECASE)


def syntax(world, scope, source, flags, line=None):
    args = [LINUX, '-std=c++26', *flags, '-fsyntax-only', source]
    cold = world.cxx(args, scope, world.cold, timeout=30)
    warm = world.cxx(args, scope, world.daemon, timeout=30)
    assert cold == warm, (args, cold, warm)
    if line is None:
        assert warm == (0, b'', b''), (args, warm)
    else:
        assert warm[0] == 1, (args, warm)
        site = re.compile(rf'{re.escape(source)}:{line}:\d+: error: (.*)')
        assert any(disabled_diagnostic(message) for message in site.findall(warm[2].decode())), (args, warm)


def command_line(world, scope):
    (scope / 'downcast.cpp').write_text(DOWNCAST)
    syntax(world, scope, 'downcast.cpp', ())
    pid = world.pid()
    for flags, enabled in MODES[1:]:
        syntax(world, scope, 'downcast.cpp', flags, None if enabled else 4)

    (scope / 'typeid.cpp').write_text(TYPEID)
    for flags, enabled in MODES:
        if not enabled:
            syntax(world, scope, 'typeid.cpp', flags, 2)
    (scope / 'features.cpp').write_text('''\
#if defined(__cpp_rtti) || defined(__GXX_RTTI)
#error advertised RTTI is unavailable under -fno-rtti
#endif
''')
    syntax(world, scope, 'features.cpp', DISABLED)
    return pid


def compile_database(world, scope):
    # The compile database supplies the flags for this unit, with unchanged editor text.
    unit = scope / 'editor.cpp'
    unit.write_text(DOWNCAST)
    database = scope / 'compile_commands.json'
    editor = Editor(world, scope)
    for version, (flags, enabled) in enumerate(MODES, 1):
        database.write_text(json.dumps([{
            'directory': str(scope), 'file': unit.name,
            'arguments': ['c++', LINUX, '-std=c++26', *flags, '-c', unit.name, '-o', 'editor.o'],
        }]))
        if version == 1:
            editor.open(unit, DOWNCAST)
        else:
            editor.change(unit, DOWNCAST, version)
        context = f'no-rtti compile database version {version}'
        found = editor.diagnostics(unit, version, context)
        if not enabled:
            assert found and any(line == 3 and severity == 1 and disabled_diagnostic(message)
                                 for line, _, severity, message in found), (context, found)
        else:
            expect(found, [], context)
    expect(editor.finish(), 0, 'no-rtti LSP shutdown')


def check_no_class_rtti(elf):
    unwanted = [name for name in elf.names
                if name.startswith(('_ZTI', '_ZTS')) or 'type_info' in name or name == '__dynamic_cast']
    assert not unwanted, (elf.path, unwanted)


def check_table(elf, enabled, bindings=(0x11,)):
    # Itanium ABI §2.5.2. The primary table's address point follows two 64-bit header words.
    table = elf.object(TABLE, 24, bindings)
    assert '_ZTV1B' not in elf.names, elf.path
    assert elf.symbol(KEY).section != 0, elf.path
    data = elf.contents(table.section)[table.value:table.value + table.size]
    slots = {16: (1, KEY, 0)}  # R_X86_64_64
    if enabled:
        assert data[:8] == b'\0' * 8, elf.path
        assert all(elf.symbol(name).section != 0 for name in DESCRIPTORS), elf.path
        slots[8] = (1, '_ZTI1D', 0)
    else:
        assert data[:16] == b'\0' * 16, elf.path
        check_no_class_rtti(elf)
    elf.check_words(table, slots)
    return table.info, table.other


def check_throw(elf):
    assert elf.symbol('_ZTIi').section == 0, elf.path
    targets = {row.symbol for rows in elf.relocations.values() for row in rows}
    assert {'_ZTIi', '__cxa_throw'} <= targets, (elf.path, targets)


def linux_objects(world, scope, libycxx, host):
    pipeline = Path(__file__).resolve().parents[1] / 'unit/pipeline'
    sys.path.insert(0, str(pipeline))
    from linux_owned_derived_vtable import Elf

    def object_file(source, output, *flags, resident=False):
        args = [LINUX, '-std=c++26', *flags, '-c', source, '-o', output]
        obj = scope / output
        images = []
        for env in (world.cold, world.daemon) if resident else (world.cold,):
            obj.unlink(missing_ok=True)
            result = world.cxx(args, scope, env, timeout=30)
            assert result == (0, b'', b''), (args, result)
            images.append(obj.read_bytes())
        assert images[0] == images[-1], args
        return Elf(obj)

    def native(*args):
        result = subprocess.run(args, cwd=scope, env=world.cold, capture_output=True, timeout=30)
        assert result.returncode == 0, (args, result)

    fixtures = Path(__file__).resolve().with_suffix('')
    owned = pipeline / 'linux_owned_derived_vtable'
    for name in ('base.hpp', 'key.cpp'):
        (scope / name).write_bytes((owned / name).read_bytes())
    for name in ('types.hpp', 'casts.cpp', 'peer.cpp'):
        (scope / name).write_bytes((fixtures / name).read_bytes())
    snapshots = {}
    ownership = []
    for flags, enabled in MODES:
        elf = object_file('key.cpp', 'owner.o', *flags, resident=True)
        ownership.append(check_table(elf, enabled=enabled))
        assert elf.data == snapshots.setdefault(enabled, elf.data), flags
    assert snapshots[True] != snapshots[False]
    assert all(entry == ownership[0] for entry in ownership), ownership

    (scope / 'throw.cpp').write_text(THROW)
    check_throw(object_file('throw.cpp', 'throw.o', *DISABLED))
    pinned_flags = ('-ffreestanding', '-nostdinc', '-fno-exceptions', *DISABLED, '-isystem', libycxx / 'include')
    marker = object_file(libycxx / 'src/runtime/linkage/static.cpp', 'marker.o', *pinned_flags)
    mark = marker.symbol('__ycxx_linkage_static_v1')
    assert mark.info >> 4 == 1 and mark.other & 3 == 2 and mark.section != 0, mark
    pal = object_file(libycxx / 'src/freestanding/pal/debugger_present.cpp', 'debugger.o', *pinned_flags)
    assert pal.symbol('ycxx_pal_debugger_present').section != 0, pal.path

    native_flags = (host, '-std=c++26', '-pedantic-errors', '-fno-exceptions', *DISABLED, '-fno-lto', '-fno-pie')
    native(*native_flags, '-c', 'peer.cpp', '-o', 'peer.o')
    native(*native_flags, '-c', owned / 'dispatch.cpp', '-o', 'dispatch.o')
    for flags, enabled in (((), True), (DISABLED, False)):
        (scope / 'owner.o').write_bytes(snapshots[enabled])
        check_no_class_rtti(object_file('casts.cpp', 'casts.o', *flags))
        native(host, '-fno-lto', '-no-pie', 'peer.o', 'dispatch.o', 'owner.o', 'casts.o',
               'marker.o', 'debugger.o', '-o', 'program')
        native('./program')

    borrowed = pipeline / 'linux_dynamic_cast_void'
    (scope / 'borrowed').mkdir()
    for name in ('base.hpp', 'consumer.cpp'):
        (scope / 'borrowed' / name).write_bytes((borrowed / name).read_bytes())
    check_no_class_rtti(object_file('borrowed/consumer.cpp', 'borrowed.o', *DISABLED))
    native(*native_flags, '-c', borrowed / 'peer.cpp', '-o', 'borrowed-peer.o')
    native(host, '-fno-lto', '-no-pie', 'borrowed.o', 'borrowed-peer.o', '-o', 'borrowed-program')
    native('./borrowed-program')


def main():
    ycxx, libycxx = (Path(arg).resolve() for arg in sys.argv[1:3])
    world = Env(str(ycxx))
    scope = world.scope.resolve()
    for env in (world.base, world.cold, world.daemon):
        env.update(LC_ALL='C', YCXX_THREADS='2')
    try:
        pid = command_line(world, scope)
        compile_database(world, scope)
        # PARITY: by design: the table and mixed consumer checks use Linux x86-64 ELF.
        if NATIVE_LINUX:
            linux_objects(world, scope, libycxx, sys.argv[3])
        world.same_daemon('no-rtti mode and compile database', pid)
    finally:
        world.close()
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
