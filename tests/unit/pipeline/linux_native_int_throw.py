"""Exercise a cleanup-free ycxx int throw with a hosted C++ caller."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from linux_cleanup_free_unwind import object_frames


LINUX = 'x86_64-pc-linux-gnu'
DARWIN = 'arm64-apple-darwin'
LOWER_THROW = 'lower: `throw` is not in the α subset'


def check_object(run, readelf, path, expected):
    _, functions, covered, _ = object_frames(path)
    assert functions.keys() == expected.keys(), (functions, expected)
    ranges = {name: (start, start + size) for name, (_, start, size) in functions.items()}
    assert all(binding == 1 for binding, _, _ in functions.values()), functions
    assert set(covered) == {(start, end - start) for start, end in ranges.values()}, covered

    metadata = run(readelf, '--wide', '--section-headers', '--relocs', '--symbols', path).stdout
    assert '.gcc_except_table' not in metadata and '__gxx_personality' not in metadata, metadata
    relocations = []
    for line in metadata.splitlines():
        if match := re.fullmatch(r'\s*([0-9a-f]+)\s+[0-9a-f]+\s+(R_X86_64_\w+)\s+'
                                 r'[0-9a-f]+\s+(\S+)\s+-\s+4\s*', line):
            at, kind, symbol = match.groups()
            relocations.append((int(at, 16), kind, symbol))

    frames = run(readelf, '--wide', '--debug-dump=frames-interp', path).stdout
    rows = {}
    for line in frames.splitlines():
        if match := re.search(r'\bFDE\b.*pc=([0-9a-f]+)\.\.([0-9a-f]+)', line):
            region = tuple(int(value, 16) for value in match.groups())
            rows[region] = []
        elif rows and (match := re.fullmatch(r'([0-9a-f]+)\s+(\S+)\s+(\S+)\s+(\S+)\s*', line)):
            at, *state = match.groups()
            rows[region].append((int(at, 16), state))

    for name, (start, end) in ranges.items():
        actual = [(at, kind, symbol) for at, kind, symbol in relocations if start <= at < end]
        assert [(kind, symbol) for _, kind, symbol in actual] == expected[name], (name, actual)
        for at, kind, symbol in actual:
            if kind != 'R_X86_64_PLT32':
                continue
            # The relocation field is the last four bytes of a direct call. The unwind PC
            # is the return address, where the function's frame pointer must still be live.
            return_pc = at + 4
            assert return_pc < end, (name, symbol, return_pc, end)
            state = [state for offset, state in rows[(start, end)] if offset <= return_pc][-1]
            assert state == ['rbp+16', 'c-16', 'c-8'], (name, symbol, state, frames)


def main():
    ycxx, peer, readelf = sys.argv[1:]
    fixtures = Path(__file__).with_suffix('').resolve()
    with tempfile.TemporaryDirectory(prefix='ycxx-throw-', dir='.') as temporary, \
            tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temporary).resolve()
        (scope / '.ycxx').touch()
        env = {key: value for key, value in os.environ.items()
               if key not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT')}
        env.update(LC_ALL='C', SDKROOT='', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='1')
        cold = dict(env, YCXX_NO_DAEMON='1')
        daemon = dict(env, YCXX_DAEMON='require')

        def run(*args, context=cold, status=0):
            result = subprocess.run(args, cwd=scope, env=context, timeout=30, capture_output=True, text=True)
            assert result.returncode == status, (args, result.returncode, status, result.stdout, result.stderr)
            return result

        def compile(*args, target=LINUX, context=cold, status=0):
            return run(ycxx, 'cxx', f'--target={target}', '-std=c++26', *args, context=context, status=status)

        def compile_object(source, output, target=LINUX, context=cold, rejection=None):
            (scope / output).unlink(missing_ok=True)
            result = compile('-c', source, '-o', output, target=target, context=context, status=int(rejection is not None))
            if rejection is None:
                assert (scope / output).stat().st_size > 0, (source, output)
            else:
                assert ': error:' in result.stderr and rejection in result.stderr, (source, result.stderr)
                assert not (scope / output).exists(), (source, output, result.stderr)
            return result.stderr

        compile_object(fixtures / 'native/authored.cpp', 'authored.o')
        compile_object(fixtures / 'native/bridge.cpp', 'bridge.o')
        plt, got = 'R_X86_64_PLT32', 'R_X86_64_REX_GOTPCRELX'
        throwing = [(plt, '__cxa_allocate_exception'), (got, '_ZTIi'), (plt, '__cxa_throw')]
        check_object(run, readelf, scope / 'authored.o', {
            'authored_choice': throwing,
            'authored_operand': [(plt, 'hosted_operand'), *throwing],
        })
        check_object(run, readelf, scope / 'bridge.o', {'authored_bridge': [(plt, 'authored_choice')]})
        run(peer, '-std=c++26', '-pedantic-errors', '-nostdinc', '-nostdinc++', '-O0', '-fexceptions',
            '-fPIE', '-fno-lto', '-c', fixtures / 'native/hosted.cpp', '-o', 'hosted.o')
        run(peer, '-pie', '-fno-lto', 'hosted.o', 'authored.o', 'bridge.o', '-o', 'hosted')
        run('./hosted')

        rejected = {
            'active-cleanup': LOWER_THROW,
            'enum': LOWER_THROW,
            'pointer': LOWER_THROW,
            'retained-handler': 'with a retained try block',
            'rethrow': 'lower: `rethrow` is not in the α subset',
            'unsigned': LOWER_THROW,
        }
        for target in (LINUX, DARWIN):
            for name, reason in rejected.items():
                source = fixtures / 'scope' / f'{name}.cpp'
                compile('-fsyntax-only', source, target=target)
                compile_object(source, 'scope.o', target=target, rejection=reason)
            compile_object(fixtures / 'scope/normal-only.cpp', 'normal.o', target=target)
        compile('-fsyntax-only', fixtures / 'native/authored.cpp', target=DARWIN)
        compile_object(fixtures / 'native/authored.cpp', 'darwin.o', target=DARWIN, rejection=LOWER_THROW)
        compile_object(fixtures / 'native/bridge.cpp', 'darwin-bridge.o', target=DARWIN)

        previous_pid, previous_requests = None, 0
        initial_object = None
        try:
            for name, rejection in (('ordinary', None), ('compatible-late', LOWER_THROW),
                                    ('nonthrowing', LOWER_THROW), ('ordinary', None)):
                shutil.copyfile(fixtures / 'warm' / f'{name}.cpp', scope / 'edit.cpp')
                compile('-fsyntax-only', 'edit.cpp')
                cold_error = compile_object('edit.cpp', 'edit.o', rejection=rejection)
                cold_object = None if rejection else (scope / 'edit.o').read_bytes()
                warm_error = compile_object('edit.cpp', 'edit.o', context=daemon, rejection=rejection)
                warm_object = None if rejection else (scope / 'edit.o').read_bytes()
                assert (warm_error, warm_object) == (cold_error, cold_object), (name, warm_error, cold_error)
                if name == 'ordinary':
                    if initial_object is None:
                        initial_object = cold_object
                    assert cold_object == initial_object
                status = run(ycxx, 'ctl', 'status', context=env).stdout
                fields = dict(re.findall(r'^(pid|requests): ([0-9]+)$', status, re.MULTILINE))
                pid, requests = int(fields['pid']), int(fields['requests'])
                assert pid > 0 and (previous_pid is None or pid == previous_pid), status
                assert requests > previous_requests, status
                previous_pid, previous_requests = pid, requests
        finally:
            subprocess.run([ycxx, 'ctl', 'shutdown'], cwd=scope, env=env, timeout=30, capture_output=True)


if __name__ == '__main__':
    main()
