"""Check that immediate functions need no native entry point."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    ycxx, readelf = sys.argv[1:]
    source = Path(__file__).resolve().with_suffix('.cpp')
    target = '--target=x86_64-pc-linux-gnu'
    env = dict(os.environ, LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='1')
    with tempfile.TemporaryDirectory(prefix='ycxx-immediate-', dir='.') as scope:
        def run(*args, status=0):
            result = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=True, timeout=30)
            if result.returncode != status:
                raise AssertionError(f'{args} exited {result.returncode}, expected {status}\n'
                                     f'{result.stdout}{result.stderr}')
            return result.stdout

        run(ycxx, 'cxx', target, '-c', source, '-o', 'immediate.o')
        table = run(readelf, '-Ws', '--demangle', 'immediate.o')
        symbols = {row[-1]: row for line in table.splitlines()
                   if len(row := line.split(maxsplit=7)) == 8 and row[0].endswith(':')}
        assert 'unused_empty()' not in symbols, table
        assert 'demanded_empty()' not in symbols, table
        runtime = symbols.get('runtime_step(int)')
        assert runtime and runtime[3] == 'FUNC' and runtime[6].isdecimal(), table

        run(ycxx, 'cxx', target, '-nostdlib', 'immediate.o', '-o', 'program')
        run('./program', status=8)
        run('./program', 'one', 'two', status=10)


if __name__ == '__main__':
    main()
