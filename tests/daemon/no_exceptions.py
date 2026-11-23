"""Check exception mode in CLI, same-daemon, and compilation database requests (#980)."""

import json
from pathlib import Path
import re
import subprocess
import sys

from lsp import Editor, expect
from process import Env, failures
from system_includes import LINUX, NATIVE_LINUX


DISABLED = ('-fno-exceptions',)
MODE_REPLAY = ((), DISABLED, ())
THROW = 'void probe() {\n  throw 17;\n}\n'
TEMPLATE = '''\
template <bool Enabled> int choose() {
  if constexpr (Enabled) {
    throw 17;
  } else {
    return 17;
  }
}
'''
TEMPLATE_TRY = TEMPLATE.replace('throw 17;', 'try { return 2; } catch (...) { return 3; }')
LAMBDA = '''\
int probe() {
  auto choose = [](auto) -> int {
    if constexpr (@SELECT@) {
      throw 17;
    } else {
      return 17;
    }
  };
  return choose(0);
}
'''
REJECTED = {
    'throw.cpp': (THROW, 2),
    'rethrow.cpp': ('void probe() { throw; }\n', 1),
    'try.cpp': ('int probe() { try { return 2; } catch (...) { return 3; } }\n', 1),
    'discarded.cpp': ('void probe() { if constexpr (false) { throw 17; } }\n', 1),
    'unevaluated.cpp': ('using Result = decltype(throw 17);\n', 1),
    'selected-throw.cpp': (TEMPLATE + 'int probe() { return choose<true>(); }\n', 3),
    'selected-try.cpp': (TEMPLATE_TRY + 'int probe() { return choose<true>(); }\n', 3),
    'selected-lambda.cpp': (LAMBDA.replace('@SELECT@', 'true'), 4),
}
ACCEPTED = {
    'dormant.cpp': TEMPLATE + TEMPLATE_TRY.replace('choose', 'choose_with_try'),
    'unselected.cpp': TEMPLATE + TEMPLATE_TRY.replace('choose', 'choose_with_try')
    + 'int probe() { return choose<false>() + choose_with_try<false>(); }\n',
    'unselected-lambda.cpp': LAMBDA.replace('@SELECT@', 'false'),
    'noexcept.cpp': '''\
int promised() noexcept;
int ordinary();
static_assert(noexcept(promised()));
static_assert(!noexcept(ordinary()));
''',
}


def disabled_diagnostic(message):
    return re.search(r'exception|throw|\btry\b', message, re.IGNORECASE) and re.search(
        r'disabl|not enabled|turned off|without exceptions|-fno-exceptions', message, re.IGNORECASE)


def syntax(world, scope, source, flags=(), line=None):
    args = [LINUX, '-std=c++26', *flags, '-fsyntax-only', source]
    cold = world.cxx(args, scope, world.cold, timeout=30)
    warm = world.cxx(args, scope, world.daemon, timeout=30)
    assert warm == cold, (args, cold, warm)
    if line is None:
        assert warm == (0, b'', b''), (args, warm)
    else:
        assert warm[0] == 1, (args, warm)
        site = re.compile(rf'{re.escape(source)}:{line}:\d+: error: (.*)')
        matches = site.findall(warm[2].decode())
        assert any(disabled_diagnostic(message) for message in matches), (args, matches, warm)


def command_line(world, scope):
    for source, (text, _) in REJECTED.items():
        (scope / source).write_text(text)
    for source, text in ACCEPTED.items():
        (scope / source).write_text(text)

    # Start the scratch daemon with an unchanged source before capturing its PID.
    syntax(world, scope, 'throw.cpp')
    pid = world.pid()
    for flags in MODE_REPLAY[1:]:
        syntax(world, scope, 'throw.cpp', flags, line=2 if flags else None)

    # A virtual declaration selects the whole-unit path without moving either throw.
    for source in ('throw.cpp', 'selected-throw.cpp'):
        text, line = REJECTED[source]
        whole_unit = 'whole-unit-' + source
        (scope / whole_unit).write_text(text + '\nstruct ModeProbe { virtual int f(); };\n')
        for flags in MODE_REPLAY:
            syntax(world, scope, whole_unit, flags, line=line if flags else None)

    for source, (_, line) in REJECTED.items():
        if source != 'throw.cpp':
            syntax(world, scope, source)
            syntax(world, scope, source, DISABLED, line)
    for source in ACCEPTED:
        for flags in ((), DISABLED):
            syntax(world, scope, source, flags)

    (scope / 'feature.cpp').write_text('''\
#ifdef __cpp_exceptions
#error __cpp_exceptions advertised before full exception support
#endif
''')
    for flags in ((), DISABLED):
        syntax(world, scope, 'feature.cpp', flags)
    return pid


def compile_database(world, scope):
    # Keep this file out of CLI builds so each database entry supplies its flags.
    unit = scope / 'editor.cpp'
    unit.write_text(THROW)
    database = scope / 'compile_commands.json'
    editor = Editor(world, scope)
    for version, flags in enumerate(MODE_REPLAY, 1):
        database.write_text(json.dumps([{
            'directory': str(scope), 'file': unit.name,
            'arguments': ['c++', LINUX, '-std=c++26', *flags, '-c', unit.name, '-o', 'editor.o'],
        }]))
        if version == 1:
            editor.open(unit, THROW)
        else:
            editor.change(unit, THROW, version)
        context = f'no-exceptions compile database version {version}'
        found = editor.diagnostics(unit, version, context)
        if flags:
            assert found and any(line == 1 and severity == 1 and disabled_diagnostic(message)
                                 for line, _, severity, message in found), (context, found)
        else:
            expect(found, [], context)
    expect(editor.finish(), 0, 'no-exceptions LSP shutdown')


def linux_object(world, scope, libycxx, readelf):
    fixtures = Path(__file__).resolve().with_suffix('')
    # Use one project root for all commands, including the header-only library consumer.
    for name in ('native.cpp', 'peer.cpp'):
        (scope / name).write_bytes((fixtures / name).read_bytes())
    flags = [LINUX, '-std=c++26', '-ffreestanding', *DISABLED, '-nostdinc', '-isystem', libycxx / 'include']
    args = [*flags, '-I', libycxx / 'src/freestanding/new', '-c', 'native.cpp', '-o', 'native.o']
    obj = scope / 'native.o'
    images = []
    for env in (world.cold, world.daemon):
        obj.unlink(missing_ok=True)
        result = world.cxx(args, scope, env, timeout=30)
        assert result == (0, b'', b''), (args, result)
        images.append(obj.read_bytes())
    assert images[0] == images[1]

    def metadata(*args):
        result = subprocess.run([readelf, '-W', *args, obj], cwd=scope, env=world.cold,
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result
        return result.stdout

    symbols = metadata('--sections', '--symbols', '--relocs')
    for name in ('work', 'mark_cleanup', 'make_value', 'foreign'):
        assert re.search(rf'\bUND\s+{name}$', symbols, re.MULTILINE), (name, symbols)
    assert '.eh_frame' in symbols and '.gcc_except_table' not in symbols, symbols
    assert not re.search(r'__gxx_personality|_Unwind_|__cxa_(?:throw|rethrow|allocate_exception|begin_catch|end_catch)',
                         symbols), symbols
    plain, = (row.split() for row in symbols.splitlines() if row.split()[-1:] == ['plain_frame'])
    start, size = int(plain[1], 16), int(plain[2])
    frames = metadata('--debug-dump=frames')
    assert set(re.findall(r'Augmentation:\s+"([^"]*)"', frames)) == {'zR'}, frames
    ranges = {(int(low, 16), int(high, 16))
              for low, high in re.findall(r'\bFDE\b[^\n]*\bpc=([0-9a-f]+)\.\.([0-9a-f]+)', frames)}
    assert size > 0 and (start, start + size) in ranges, (plain, frames)

    # The header's static linkage guard refers to the marker provided by this pinned source.
    args = [*flags, '-c', libycxx / 'src/runtime/linkage/static.cpp', '-o', 'marker.o']
    result = world.cxx(args, scope, world.cold, timeout=30)
    assert result == (0, b'', b''), (args, result)
    args = [LINUX, '-std=c++26', *DISABLED, '-nostdlib', 'peer.cpp', 'native.o', 'marker.o', '-o', 'program']
    result = world.cxx(args, scope, world.cold, timeout=30)
    assert result == (0, b'', b''), (args, result)
    program = subprocess.run([scope / 'program'], cwd=scope, env=world.cold, capture_output=True, timeout=30)
    assert program.returncode == 0, program


def main():
    ycxx, libycxx = (Path(arg).resolve() for arg in sys.argv[1:3])
    world = Env(str(ycxx))
    scope = world.scope.resolve()
    for env in (world.base, world.cold, world.daemon):
        env.update(LC_ALL='C', YCXX_THREADS='2')
    try:
        pid = command_line(world, scope)
        compile_database(world, scope)
        # PARITY: by design: this checks x86-64 ELF metadata and runs an x86-64 ELF executable.
        if NATIVE_LINUX:
            linux_object(world, scope, libycxx, sys.argv[3])
        world.same_daemon('no-exceptions mode and compile database', pid)
    finally:
        world.close()
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
