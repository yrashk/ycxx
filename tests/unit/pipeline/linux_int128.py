"""Run the 128-bit integer tests on x86-64 Linux, with native peers and runtimes (#1172)."""

import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run
from linux_peer_derived_default_construction import native_compilers


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
        for compiler in native_compilers(host, scope, env):
            for level in ('-O0', '-O2'):
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
