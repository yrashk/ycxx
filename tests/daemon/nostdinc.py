"""Check explicit include paths without implicit header directories (#251)."""
import json
import sys
from pathlib import Path

from lsp import Editor, expect
from process import Env, failures
from system_includes import DARWIN, LINUX, check


def command_line(world, scope, resources):
    (scope / 'cli.cpp').write_text('''\
#include <stddef.h>
static_assert(sizeof(size_t) == sizeof(void*));
#ifdef EXPECT_EXPLICIT
static_assert(YCXX_EXPLICIT_HEADER == 42);
#endif
''')
    target = (LINUX, '-std=c++26')
    missing = b'cli.cpp:1:10: error: file not found'
    check(world, scope, *target, 'cli.cpp')
    pid = world.pid()
    check(world, scope, *target, '-nostdinc', 'cli.cpp', error=missing)
    for flag in ('-I', '-isystem'):
        check(world, scope, *target, '-nostdinc', flag, 'explicit', '-DEXPECT_EXPLICIT', 'cli.cpp')
    check(world, scope, *target, '-nostdinc', '-isystem', resources, 'cli.cpp')
    check(world, scope, *target, '-nostdinc', 'cli.cpp', error=missing)
    check(world, scope, *target, 'cli.cpp')

    sdk = scope / 'sdk/usr/include'
    sdk.mkdir(parents=True)
    (sdk / 'ycxx_nostdinc_sdk.h').write_text('#define YCXX_SDK_HEADER 42\n')
    (scope / 'sdk.cpp').write_text('#include <ycxx_nostdinc_sdk.h>\nstatic_assert(YCXX_SDK_HEADER == 42);\n')
    check(world, scope, DARWIN, 'sdk.cpp')
    check(world, scope, DARWIN, '-nostdinc', 'sdk.cpp', error=b'sdk.cpp:1:10: error: file not found')
    return pid


def compile_database(world, scope):
    # A preceding driver build of this file would take precedence over its database entry.
    unit = scope / 'listed.cpp'
    text = '''\
#if defined(EXPECT_EXPLICIT)
#include <ycxx/pal.h>
static_assert(YCXX_EXPLICIT_HEADER == 42);
#elif !__has_include(<ycxx/pal.h>)
#include <ycxx/pal.h>
#endif
'''
    unit.write_text(text)
    database = scope / 'compile_commands.json'
    editor = Editor(world, scope)
    missing = [(4, 9, 1, 'file not found')]
    for version, (flags, expected) in enumerate((
        ([], []),
        (['-nostdinc'], missing),
        (['-nostdinc', '-I', 'explicit', '-DEXPECT_EXPLICIT'], []),
        (['-nostdinc', '-isystem', 'explicit', '-DEXPECT_EXPLICIT'], []),
        (['-nostdinc'], missing),
        ([], []),
    ), 1):
        database.write_text(json.dumps([{
            'directory': str(scope), 'file': unit.name,
            'arguments': ['c++', '-std=c++26', *flags, '-c', unit.name, '-o', 'listed.o'],
        }]))
        if version == 1:
            editor.open(unit, text)
        else:
            editor.change(unit, text, version)
        expect(editor.diagnostics(unit, version, f'compile database flags {flags}'), expected,
               f'compile database flags {flags}')
    expect(editor.finish(), 0, 'nostdinc LSP shutdown')


def main():
    ycxx, resources = (Path(arg).resolve() for arg in sys.argv[1:])
    world = Env(str(ycxx))
    scope = world.scope.resolve()
    for env in (world.base, world.cold, world.daemon):
        env.update(LC_ALL='C', SDKROOT='sdk', YCXX_THREADS='2')
    try:
        (scope / 'explicit/ycxx').mkdir(parents=True)
        marker = '#define YCXX_EXPLICIT_HEADER 42\n'
        (scope / 'explicit/stddef.h').write_text(marker + 'typedef __SIZE_TYPE__ size_t;\n')
        (scope / 'explicit/ycxx/pal.h').write_text(marker)
        pid = command_line(world, scope, resources)
        compile_database(world, scope)
        world.same_daemon('nostdinc command line and compile database', pid)
    finally:
        world.close()
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
