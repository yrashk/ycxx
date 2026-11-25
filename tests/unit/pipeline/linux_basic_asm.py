"""Check the Linux x86-64 basic asm statement profile with a native SIGTRAP peer."""

import os
from pathlib import Path
import re
import resource
import signal
import subprocess
import sys
import tempfile

from linux_compile import run
from linux_peer_derived_default_construction import native_compilers

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'daemon'))
from runtime_dir import runtime_root


def rejected(args, scope, env):
    result = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=True, timeout=30)
    assert result.returncode == 1 and ': error:' in result.stderr, result
    return result.stdout, result.stderr


def disassembly_traps(objdump, obj, scope, env):
    # Only disassemble ycxx's object. The native compiler oracle checks program behavior.
    listing = run([objdump, '-d', obj], scope, env)
    return len(re.findall(r'^\s*[0-9a-f]+:\s+cc\s+int3(?:\s|$)', listing, re.MULTILINE))


def replay(ycxx, objdump, scope, env, original):
    target = [ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26']
    cold, resident = dict(env, YCXX_NO_DAEMON='1'), dict(env, YCXX_DAEMON='require')
    source, obj = scope / 'breakpoint.cpp', scope / 'breakpoint.o'
    syntax = [*target, '-fsyntax-only', source.name]
    compile_object = [*target, '-c', source.name, '-o', obj.name]

    def build(context, traps):
        obj.unlink(missing_ok=True)
        run(syntax, scope, context)
        if traps is None:
            diagnostic = rejected(compile_object, scope, context)
            assert not obj.exists(), diagnostic
            return diagnostic
        run(compile_object, scope, context)
        assert disassembly_traps(objdump, obj.name, scope, context) == traps
        return obj.read_bytes()

    def compare(contents, traps):
        source.write_bytes(contents)
        fresh = build(cold, traps)
        assert fresh == build(resident, traps)
        return fresh

    def pid():
        status = run([ycxx, 'ctl', 'status'], scope, env)
        found, = (line for line in status.splitlines() if line.startswith('pid: '))
        return found

    try:
        baseline = compare(original, 2)
        initial_pid = pid()
        _, diagnostic = compare(original.replace(b'if constexpr (true)', b'if constexpr (false)'), None)
        assert f'{source.name}:11:7: error:' in diagnostic, diagnostic
        # The entire decoded instruction must match, even with a valid INT3 prefix.
        compare(original.replace(b'"int3"', b'"int3; nop"', 1), None)
        compare(original.replace(b'"int3"', b'"int3\\0junk"', 1), None)
        compare(original.replace(b'"int3"', b'"in\\1643"', 1), 2)
        compare(original.replace(b'asm("int3");', b'(void)0;'), 1)
        source.write_bytes(original.replace(b'asm("int3");', b'asm("int3")'))
        assert rejected(syntax, scope, cold) == rejected(syntax, scope, resident)

        constant = scope / 'constant.cpp'
        sample = ('constexpr int value(bool selected) { if (selected) asm("int3"); return 23; }\n'
                  'static_assert(value(false) == 23);\n')
        constant.write_text(sample)
        constant_syntax = [*target, '-fsyntax-only', constant.name]
        for context in (cold, resident):
            run(constant_syntax, scope, context)
        constant.write_text(sample.replace('value(false)', 'value(true)'))
        assert rejected(constant_syntax, scope, cold) == rejected(constant_syntax, scope, resident)

        assert compare(original, 2) == baseline and pid() == initial_pid
    finally:
        subprocess.run([ycxx, 'ctl', 'shutdown'], cwd=scope, env=env, capture_output=True, timeout=30)


def check_signal_paths(program, scope, env):
    for mode, expected in (('inactive', 0), ('direct', -signal.SIGTRAP),
                           ('selected', -signal.SIGTRAP), ('handled', 0)):
        result = subprocess.run([program, mode], cwd=scope, env=env, capture_output=True, timeout=10)
        assert result.returncode == expected, (mode, expected, result)


def main():
    ycxx, host, objdump = sys.argv[1:]
    fixture = Path(__file__).resolve().with_suffix('')
    original = (fixture / 'breakpoint.cpp').read_bytes()
    peer = fixture / 'peer.cpp'
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='ycxx-basic-asm-', dir='.') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-', dir=runtime_root('yd-')) as runtime:
        scope = Path(temp).resolve()
        (scope / '.ycxx').touch()
        env = {key: value for key, value in os.environ.items()
               if key not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT', 'YCXX_NO_WATCH')}
        env.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2', XDG_CACHE_HOME=str(scope / 'cache'))
        replay(ycxx, objdump, scope, env, original)
        for compiler in native_compilers(host, scope, env):
            run([compiler, '-std=c++20', '-pedantic-errors', '-O2', '-fno-lto', '-fPIE',
                 '-c', peer, '-o', 'peer.o'], scope, env)
            run([compiler, '-pie', '-fno-lto', '-Wl,-z,text', 'breakpoint.o', 'peer.o', '-o', 'mixed'], scope, env)
            check_signal_paths('./mixed', scope, env)


if __name__ == '__main__':
    main()
