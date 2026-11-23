"""Check the freestanding warning options in the CLI and daemon (#1014)."""

import json
from pathlib import Path
import sys

from lsp import Editor, expect
from process import Env, failures
from system_includes import LINUX


LITERAL = "int warning_probe() { return 'ab'; }\n"
PP_LITERAL = "#if 'ab'\nint preprocessor_probe = 17;\n#endif\n"
REQUIRED = '#warning retained_required_warning\n#error retained_required_error\n'
SYSTEM_HEADER = '#warning retained_system_warning\n' + LITERAL
PIN_FLAGS = ('-Wall', '-Wextra', '-Wno-sized-deallocation', '-Wno-attributes')
UNKNOWN = '-Wycxx-spec-unknown-warning'
TARGET = (LINUX, '-std=c++26')

# The group controls the default warning; specific controls override it in either order.
MODES = (
    ((), True),
    (('-Wno-all',), False),
    (('-Wno-all', '-Wall'), True),
    (('-Wall', '-Wno-all'), False),
    (('-Wmultichar', '-Wno-all'), True),
    (('-Wno-all', '-Wmultichar'), True),
    (('-Wno-multichar', '-Wall'), False),
    (('-Wall', '-Wno-multichar'), False),
    (('-Wno-multichar', '-Wmultichar'), True),
    (('-Wmultichar', '-Wno-multichar'), False),
    (('-Wno-all', '-Wextra'), False),
    (('-Wno-extra',), True),
    ((), True),
)


def warnings(diagnostics):
    return [line for line in diagnostics.splitlines() if line.startswith(b'warning:') or b': warning:' in line]


def command_line(world, scope):
    for name, text in (('literal.cpp', LITERAL), ('pp.cpp', PP_LITERAL),
                       ('required.cpp', REQUIRED), ('clean.cpp', 'int clean = 0;\n'),
                       ('system.cpp', '#include <warning.hpp>\n')):
        (scope / name).write_text(text)
    (scope / 'inc').mkdir()
    (scope / 'inc/warning.hpp').write_text(SYSTEM_HEADER)

    def invoke(source, *flags, phase='-fsyntax-only', status=0):
        args = [*TARGET, *flags, phase, source]
        cold = world.cxx(args, scope, world.cold, timeout=30)
        warm = world.cxx(args, scope, world.daemon, timeout=30)
        assert cold == warm and cold[0] == status, (args, cold, warm)
        return cold[1], cold[2]

    reports = []
    for flags, enabled in MODES:
        _, diagnostic = invoke('literal.cpp', *flags)
        emitted = warnings(diagnostic)
        assert len(emitted) == int(enabled), (flags, diagnostic)
        assert all(b'literal.cpp:1:' in line for line in emitted), diagnostic
        if not reports:
            pid = world.pid()
        reports.append(diagnostic)
    assert reports[0] == reports[-1]

    # Preprocess-only compilation does not publish the phase-4 literal warning.
    for flags in ((), ('-Wno-all',), ('-Wmultichar', '-Wno-all'), ('-Wno-multichar',)):
        tokens, diagnostic = invoke('pp.cpp', *flags, phase='-E')
        assert b'preprocessor_probe' in tokens, tokens
        assert not warnings(diagnostic), (flags, diagnostic)

    # The pinned switches and their positive counterparts must accept a quiet input.
    for flags in (PIN_FLAGS, ('-Wno-extra', '-Wsized-deallocation', '-Wattributes')):
        _, diagnostic = invoke('clean.cpp', *flags)
        assert not diagnostic, (flags, diagnostic)

    for flag in (UNKNOWN, UNKNOWN.replace('-W', '-Wno-', 1)):
        _, diagnostic = invoke('clean.cpp', flag, status=1)
        assert b'unsupported option' in diagnostic and flag.encode() in diagnostic, diagnostic

    _, diagnostic = invoke('required.cpp', '-Wno-all', '-Wno-extra', '-Wno-multichar', phase='-E', status=1)
    assert any(b'retained_required_warning' in line for line in warnings(diagnostic)), diagnostic
    assert any(b': error:' in line and b'retained_required_error' in line
               for line in diagnostic.splitlines()), diagnostic

    for include, expected in (('-I', 2), ('-isystem', 1)):
        _, diagnostic = invoke('system.cpp', '-Wno-all', '-Wmultichar', include, 'inc')
        emitted = warnings(diagnostic)
        assert len(emitted) == expected, (include, diagnostic)
        assert any(b'warning.hpp:1:' in line and b'#warning' in line for line in emitted), diagnostic
        assert any(b'warning.hpp:2:' in line for line in emitted) == (include == '-I'), diagnostic
    return pid


def compile_database(world, scope):
    # This unit has no prior CLI request, so the database supplies its options.
    unit = scope / 'editor.cpp'
    unit.write_text(LITERAL)
    database = scope / 'compile_commands.json'
    editor = Editor(world, scope)
    reports = []
    modes = (*MODES[:-1], (('-Wno-all', UNKNOWN), False), MODES[-1])
    for version, (flags, enabled) in enumerate(modes, 1):
        database.write_text(json.dumps([{
            'directory': str(scope), 'file': unit.name,
            'arguments': ['c++', *TARGET, *flags, '-c', unit.name, '-o', 'editor.o'],
        }]))
        if version == 1:
            editor.open(unit, LITERAL)
        else:
            editor.change(unit, LITERAL, version)
        context = f'warning option database version {version}'
        diagnostics = editor.diagnostics(unit, version, context)
        assert diagnostics is not None and len(diagnostics) == int(enabled), (context, flags, diagnostics)
        assert all(line == 0 and severity == 2 for line, _, severity, _ in diagnostics), (context, diagnostics)
        reports.append(diagnostics)
    assert reports[0] == reports[-1]
    expect(editor.finish(), 0, 'warning option LSP shutdown')


def main():
    world = Env(str(Path(sys.argv[1]).resolve()))
    scope = world.scope.resolve()
    for env in (world.base, world.cold, world.daemon):
        env.update(LC_ALL='C', YCXX_THREADS='2')
    try:
        pid = command_line(world, scope)
        compile_database(world, scope)
        world.same_daemon('warning options and compile database', pid)
    finally:
        world.close()
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
