"""Accept the explicit default mode for automatic scalar initialization (#1015)."""

import json
from pathlib import Path
import sys

from lsp import Editor, expect
from process import Env, failures
from system_includes import LINUX


FLAG = '-ftrivial-auto-var-init=uninitialized'
MODES = ((), (FLAG,), ())
TARGET = (LINUX, '-std=c++26')
SOURCE = '''\
extern "C" unsigned auto_var_probe(unsigned input) {
  unsigned initialized = input + 7u;
  unsigned assigned;
  assigned = input ^ 3u;
  return initialized + assigned;
}
'''
INVALID = 'int diagnostic_probe() { return missing_auto_var_probe(); }\n'


def command_line(world, scope):
    (scope / 'scalar.cpp').write_text(SOURCE)
    (scope / 'invalid.cpp').write_text(INVALID)
    obj = scope / 'scalar.o'

    def invoke(*args, status=0):
        args = [*TARGET, *args]
        cold = world.cxx(args, scope, world.cold, timeout=30)
        warm = world.cxx(args, scope, world.daemon, timeout=30)
        assert cold == warm and cold[0] == status, (args, cold, warm)
        return cold

    snapshots = []
    for flags in MODES:
        args = [*TARGET, *flags, '-c', 'scalar.cpp', '-o', obj.name]
        images = []
        for env in (world.cold, world.daemon):
            obj.unlink(missing_ok=True)
            result = world.cxx(args, scope, env, timeout=30)
            assert result == (0, b'', b''), (args, result)
            images.append(obj.read_bytes())
        assert images[0] == images[1] and images[0].startswith(b'\x7fELF'), flags
        if not snapshots:
            pid = world.pid()
        snapshots.append(images[0])
    assert snapshots[0] == snapshots[1] == snapshots[2]

    diagnostic = invoke('-fsyntax-only', 'invalid.cpp', status=1)
    assert b'invalid.cpp:1:' in diagnostic[2] and b'missing_auto_var_probe' in diagnostic[2], diagnostic
    assert invoke(FLAG, '-fsyntax-only', 'invalid.cpp', status=1) == diagnostic

    for suffix in ('=zero', '=pattern', '', '=', '=uninitialized-extra'):
        option = '-ftrivial-auto-var-init' + suffix
        obj.unlink(missing_ok=True)
        _, _, diagnostic = invoke(option, '-c', 'scalar.cpp', '-o', obj.name, status=1)
        assert b'unsupported option' in diagnostic and option.encode() in diagnostic, diagnostic
        assert not obj.exists(), obj
    return pid


def compile_database(world, scope):
    unit = scope / 'editor.cpp'
    text = SOURCE + '#ifdef CHECK_DATABASE\n' + INVALID + '#endif\n'
    unit.write_text(text)
    database = scope / 'compile_commands.json'
    editor = Editor(world, scope)
    check = '-DCHECK_DATABASE'
    for version, (flags, invalid) in enumerate((
        ((), False), ((FLAG,), False), ((FLAG, check), True), ((check,), True), ((FLAG,), False), ((), False),
    ), 1):
        database.write_text(json.dumps([{
            'directory': str(scope), 'file': unit.name,
            'arguments': ['c++', *TARGET, *flags, '-c', unit.name, '-o', 'editor.o'],
        }]))
        if version == 1:
            editor.open(unit, text)
        else:
            editor.change(unit, text, version)
        context = f'auto-var-init compile database version {version}'
        found = editor.diagnostics(unit, version, context)
        expected = [(SOURCE.count('\n') + 1, INVALID.index('missing_auto_var_probe'), 1,
                     "use of undeclared identifier 'missing_auto_var_probe'")] if invalid else []
        expect(found, expected, context)
    expect(editor.finish(), 0, 'auto-var-init LSP shutdown')


def main():
    world = Env(str(Path(sys.argv[1]).resolve()))
    scope = world.scope.resolve()
    for env in (world.base, world.cold, world.daemon):
        env.update(LC_ALL='C', YCXX_THREADS='2')
    try:
        pid = command_line(world, scope)
        compile_database(world, scope)
        world.same_daemon('auto-var-init command line and compile database', pid)
    finally:
        world.close()
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
