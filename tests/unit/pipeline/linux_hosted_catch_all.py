"""Host-link the unwind emitter fixtures and separately compile authored catch-all sources."""

import os
from pathlib import Path
import re
import resource
import subprocess
import sys
import tempfile


def check_authored_frames(metadata, frames):
    functions = {name: (int(start, 16), int(start, 16) + int(size))
                 for start, size, name in re.findall(
                     r'^\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+\S+\s+GLOBAL\s+\S+\s+\d+\s+(chosen|transit)$',
                     metadata, re.MULTILINE)}
    assert functions.keys() == {'chosen', 'transit'}, metadata
    cies = dict(re.findall(r'^([0-9a-f]+) +[0-9a-f]+ +[0-9a-f]+ CIE\n[^\n]+\n'
                           r'[ \t]*Augmentation:[ \t]*"([^"]+)"', frames, re.MULTILINE))
    entries = re.findall(r'FDE cie=([0-9a-f]+) pc=([0-9a-f]+)\.\.([0-9a-f]+)', frames)
    assert len(entries) == 2, frames
    ranges = {(int(start, 16), int(end, 16)): cies[cie] for cie, start, end in entries}
    assert len(ranges) == 2 and ranges == {functions['chosen']: 'zPLR', functions['transit']: 'zR'}, (functions, frames)


def main():
    emitter, ycxx, peer, readelf = sys.argv[1:]
    fixtures = Path(__file__).with_suffix('').resolve()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    env = {key: value for key, value in os.environ.items()
           if key not in ('YCXX_DAEMON', 'YCXX_ROOT', 'YCXX_RUNTIME_DIR', 'SDKROOT')}
    env.update(LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='1')
    with tempfile.TemporaryDirectory(prefix='ycxx-catch-all-', dir='.') as temporary:
        work = Path(temporary).resolve()
        (work / '.ycxx').touch()

        def run(*args, status=0):
            result = subprocess.run(args, cwd=work, env=env, capture_output=True, text=True, timeout=40)
            assert result.returncode == status, (args, result.returncode, status, result.stdout, result.stderr)
            return result

        def read_unwind(name):
            metadata = run(readelf, '--wide', '--section-headers', '--relocs', '--symbols', f'{name}.o').stdout
            for expected in ('.eh_frame', '.gcc_except_table', '.data.rel.ro', 'R_X86_64_PC32',
                             'R_X86_64_64', '__gxx_personality_v0', '__cxa_begin_catch', '__cxa_end_catch',
                             '_Unwind_Resume'):
                assert expected in metadata, (name, expected, metadata)
            frames = run(readelf, '--wide', '--debug-dump=frames', f'{name}.o').stdout
            assert '"zR"' in frames and '"zPLR"' in frames, (name, frames)
            return metadata, frames

        run(emitter, work)
        orders = ('handler-first', 'plain-first')
        for name in (*orders, 'cleanup-only'):
            read_unwind(name)

        peer_flags = ('-std=c++26', '-pedantic-errors', '-nostdinc', '-nostdinc++', '-O0', '-fexceptions',
                      '-fPIE', '-fno-lto', '-c')
        run(peer, *peer_flags, fixtures / 'native/hosted.cpp', '-o', 'hosted.o')

        def run_hosted(name):
            run(peer, '-pie', '-Wl,-z,text', '-fno-lto', 'hosted.o', f'{name}.o', '-o', name)
            run(f'./{name}')

        for name in orders:
            run_hosted(name)

        run(peer, *peer_flags, fixtures / 'native/cleanup-hosted.cpp', '-o', 'cleanup-hosted.o')
        run(peer, '-pie', '-Wl,-z,text', '-fno-lto', 'cleanup-hosted.o', 'cleanup-only.o', '-o', 'cleanup-only')
        run('./cleanup-only')

        linux = '--target=x86_64-pc-linux-gnu'
        native = run(ycxx, 'cxx', linux, '-nostdlib', 'handler-first.o', '-o', 'native', status=1)
        assert 'native ELF linking of exception handlers is unsupported' in native.stderr, native.stderr
        assert not (work / 'native').exists()

        darwin = '--target=arm64-apple-darwin'
        authored = [(name, fixtures / 'native' / f'{name}.cpp') for name in orders]
        for name, source in authored:
            output = f'authored-{name}'
            flags = (ycxx, 'cxx', linux, '-std=c++26')
            run(*flags, '-fsyntax-only', source)
            run(*flags, '-c', source, '-o', f'{output}.o')
            check_authored_frames(*read_unwind(output))
            run_hosted(output)

        scope = sorted((fixtures / 'scope').glob('*.cpp'))
        for target, rejected in ((linux, scope), (darwin, scope + [source for _, source in authored])):
            for source in rejected:
                flags = (ycxx, 'cxx', target, '-std=c++26')
                run(*flags, '-fsyntax-only', source)
                blocked = run(*flags, '-c', source, '-o', 'retained.o', status=1)
                assert 'with a retained try block' in blocked.stderr, (source, blocked.stderr)
                assert not (work / 'retained.o').exists()


if __name__ == '__main__':
    main()
