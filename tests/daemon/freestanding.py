"""Check hostedness in the driver, resident daemon, and editor (#973)."""

import json
from pathlib import Path
import re
import subprocess
import sys

from lsp import Editor, expect
from process import Env, failures
from system_includes import LINUX, NATIVE_LINUX


def compile(world, scope, env, *args):
    result = world.cxx([LINUX, '-std=c++26', *args], scope, env, timeout=30)
    assert result[0] == 0 and not result[2], (args, result)
    return result[1]


def preprocess(world, scope, *args):
    cold = compile(world, scope, world.cold, '-E', *args)
    assert compile(world, scope, world.daemon, '-E', *args) == cold, args
    return cold


def object_bytes(world, scope, source, output, *flags):
    obj = scope / output
    results = []
    for env in (world.cold, world.daemon):
        obj.unlink(missing_ok=True)
        compile(world, scope, env, *flags, '-c', source, '-o', output)
        results.append(obj.read_bytes())
    assert results[0] == results[1], (source, flags)
    return results[0]


def driver_modes(world, scope, libycxx):
    include = ('-isystem', libycxx / 'include')
    (scope / 'mode.cpp').write_text('extern "C" int hosted_mode() { return __STDC_HOSTED__; }\n')
    (scope / 'config.cpp').write_text('''\
#include <ycxx/config.hpp>
#if defined(_YCXX_HOSTED) && _YCXX_HOSTED == 1
int pinned_hosted_mode = 1;
#elif defined(_YCXX_HOSTED) && _YCXX_HOSTED == 0
int pinned_hosted_mode = 0;
#else
#error unexpected pinned hosted mode
#endif
''')
    (scope / 'main.cpp').write_text('''\
#include <ycxx/pal.h>
extern "C" int hosted_mode();
int main() { return ycxx_pal_thread_self() == 1 ? hosted_mode() : 2; }
''')
    snapshots = []
    for flags, expected in (((), b'1'), (('-ffreestanding',), b'0'), ((), b'1')):
        tokens = preprocess(world, scope, *flags, 'mode.cpp')
        assert re.search(rb'\breturn\s+' + expected + rb'\s*;', tokens), tokens
        if not snapshots:
            pid = world.pid()
            object_bytes(world, scope, libycxx / 'src/freestanding/pal/thread_self.cpp', 'thread.o',
                         '-ffreestanding', *include)
        config = preprocess(world, scope, *flags, *include, 'config.cpp')
        assert re.search(rb'\bpinned_hosted_mode\s*=\s*' + expected + rb'\s*;', config), config
        snapshots.append((tokens, config, object_bytes(world, scope, 'mode.cpp', 'mode.o', *flags)))

        # PARITY: by design: this smoke check executes a native x86-64 ELF startup.
        if NATIVE_LINUX:
            program = scope / 'program'
            program.unlink(missing_ok=True)
            compile(world, scope, world.cold, *flags, *include, '-nostdlib', 'main.cpp', 'mode.o', 'thread.o',
                    '-o', program.name)
            result = subprocess.run([program], cwd=scope, env=world.cold, capture_output=True, timeout=30)
            assert result.returncode == int(expected), (flags, result)

    assert snapshots[0] == snapshots[2]
    assert snapshots[0][2] != snapshots[1][2]
    return pid


def compile_database(world, scope):
    # Keep this unit out of CLI builds so the database supplies its flags.
    unit = scope / 'editor.cpp'
    text = '''\
#if __STDC_HOSTED__
int editor_mode() {
  return needs_freestanding();
}
#else
static_assert(__STDC_HOSTED__ == 0);
#endif
'''
    unit.write_text(text)
    database = scope / 'compile_commands.json'
    editor = Editor(world, scope)
    hosted = [(2, 9, 1, "use of undeclared identifier 'needs_freestanding'")]
    for version, (flags, expected) in enumerate((((), hosted), (('-ffreestanding',), []), ((), hosted)), 1):
        database.write_text(json.dumps([{
            'directory': str(scope), 'file': unit.name,
            'arguments': ['c++', '-std=c++26', *flags, '-c', unit.name, '-o', 'editor.o'],
        }]))
        if version == 1:
            editor.open(unit, text)
        else:
            editor.change(unit, text, version)
        expect(editor.diagnostics(unit, version, f'freestanding database version {version}'), expected,
               f'freestanding database version {version}')
    expect(editor.finish(), 0, 'freestanding LSP shutdown')


def main():
    ycxx, libycxx = (Path(arg).resolve() for arg in sys.argv[1:])
    world = Env(str(ycxx))
    for env in (world.base, world.cold, world.daemon):
        env.update(LC_ALL='C', YCXX_THREADS='2')
    try:
        scope = world.scope.resolve()
        pid = driver_modes(world, scope, libycxx)
        compile_database(world, scope)
        world.same_daemon('freestanding mode and compile database', pid)
    finally:
        world.close()
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
