"""Host-link the Stage A objects before enabling source handler lowering."""

import os
from pathlib import Path
import resource
import subprocess
import sys
import tempfile


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

        run(emitter, work)
        for name in ('handler-first', 'plain-first', 'cleanup-only'):
            metadata = run(readelf, '--wide', '--section-headers', '--relocs', '--symbols', f'{name}.o').stdout
            for expected in ('.eh_frame', '.gcc_except_table', '.data.rel.ro', 'R_X86_64_PC32',
                             'R_X86_64_64', '__gxx_personality_v0', '__cxa_begin_catch', '__cxa_end_catch',
                             '_Unwind_Resume'):
                assert expected in metadata, (name, expected, metadata)
            frames = run(readelf, '--debug-dump=frames', f'{name}.o').stdout
            assert '"zR"' in frames and '"zPLR"' in frames, (name, frames)

        peer_flags = ('-std=c++26', '-pedantic-errors', '-nostdinc', '-nostdinc++', '-O0', '-fexceptions',
                      '-fPIE', '-fno-lto', '-c')
        run(peer, *peer_flags, fixtures / 'native/hosted.cpp', '-o', 'hosted.o')
        for name in ('handler-first', 'plain-first'):
            run(peer, '-pie', '-Wl,-z,text', '-fno-lto', 'hosted.o', f'{name}.o', '-o', name)
            run(f'./{name}')

        run(peer, *peer_flags, fixtures / 'native/cleanup-hosted.cpp', '-o', 'cleanup-hosted.o')
        run(peer, '-pie', '-Wl,-z,text', '-fno-lto', 'cleanup-hosted.o', 'cleanup-only.o', '-o', 'cleanup-only')
        run('./cleanup-only')

        linux = '--target=x86_64-pc-linux-gnu'
        native = run(ycxx, 'cxx', linux, '-nostdlib', 'handler-first.o', '-o', 'native', status=1)
        assert 'native ELF linking of exception handlers is unsupported' in native.stderr, native.stderr
        assert not (work / 'native').exists()

        authored = [fixtures / 'native' / f'{name}.cpp' for name in ('handler-first', 'plain-first')]
        authored.extend(sorted((fixtures / 'scope').glob('*.cpp')))
        for target in (linux, '--target=arm64-apple-darwin'):
            for source in authored:
                flags = (ycxx, 'cxx', target, '-std=c++26')
                run(*flags, '-fsyntax-only', source)
                blocked = run(*flags, '-c', source, '-o', 'retained.o', status=1)
                assert 'with a retained try block' in blocked.stderr, (source, blocked.stderr)
                assert not (work / 'retained.o').exists()


if __name__ == '__main__':
    main()
