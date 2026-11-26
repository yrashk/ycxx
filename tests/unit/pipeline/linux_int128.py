"""Run the 128-bit integer tests on x86-64 Linux, with native peers and runtimes (#1172)."""

import os
from pathlib import Path
import re
import sys
import tempfile

from linux_compile import run
from linux_peer_derived_default_construction import native_compilers

# Clang 16 to 18 pass a 128-bit integer differently from the AMD64 psABI as GCC and Clang 23 do,
# even calling GCC alone (#1368): 16 and 17 split it between the last register and the stack, 18
# leaves that register unused for the arguments after it. 19 to 22 were not measured; 23 is the
# oracle (DECISIONS §2.3). A host whose `clang++` is older is not a peer for these tests.
ORACLE_CLANG = 23


def peers(host, scope, env):
    for compiler in native_compilers(host, scope, env):
        version = run([compiler, '--version'], scope, env).splitlines()[0]
        clang = re.search(r'clang version (\d+)', version)
        if clang and int(clang[1]) < ORACLE_CLANG:
            print(f'Skipping {compiler} ({version}): older than Clang {ORACLE_CLANG} (#1368)', flush=True)
            continue
        yield compiler, version


def main():
    ycxx, host, tests = sys.argv[1:]
    env = {key: value for key, value in os.environ.items() if key not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='2')
    with tempfile.TemporaryDirectory(prefix='ycxx-int128-') as temporary:
        scope = Path(temporary)
        for source in Path(tests).iterdir():
            (scope / source.name).write_bytes(source.read_bytes())
        for test in ('arith', 'abi', 'symbols'):
            run([ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26', '-Wno-pedantic', '-c',
                 f'{test}.run.cpp', '-o', f'{test}.o'], scope, env)
        # Without a peer nothing below runs; that is a failure, not a pass.
        chosen = list(peers(host, scope, env))
        if not chosen:
            raise AssertionError(f'no peer compiler for the int128 tests: every candidate, {host} included, is older '
                                 f'than Clang {ORACLE_CLANG} (#1368); set YCXX_GXX or YCXX_CLANGXX')
        for compiler, version in chosen:
            for level in ('-O0', '-O2'):
                print(f'Checking mixed int128 tests with {compiler} ({version}) at {level}', flush=True)
                for test in ('abi', 'symbols'):
                    run([compiler, '-std=c++26', level, '-fno-lto', '-fno-pie', '-c', f'{test}-peer.cpp',
                         '-o', f'{test}-peer.o'], scope, env)
                    run([compiler, '-fno-lto', '-no-pie', f'{test}.o', f'{test}-peer.o', '-o', test], scope, env)
                    run([f'./{test}'], scope, env)
            # Division, remainder and the floating conversions come from the compiler's runtime.
            run([compiler, '-fno-lto', '-no-pie', 'arith.o', '-o', 'arith'], scope, env)
            run(['./arith'], scope, env)


if __name__ == '__main__':
    main()
