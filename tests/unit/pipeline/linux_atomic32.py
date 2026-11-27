"""Compile and run the 32-bit GNU atomic witness on Linux x86-64."""

import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run


def main():
    ycxx, = sys.argv[1:]
    fixture = Path(__file__).resolve().with_suffix('') / 'atomic32-status.cpp'
    compiler = [ycxx, 'cxx', '--no-daemon', '--target=x86_64-pc-linux-gnu',
                '-std=c++26', '-fcompat=spec']
    env = dict(os.environ, LC_ALL='C')
    with tempfile.TemporaryDirectory(prefix='ycxx-atomic32-') as temp:
        scope = Path(temp)
        (scope / 'main.cpp').write_text(
            'extern "C" int atomic32_status();\n'
            'int main() { return atomic32_status(); }\n')
        run([*compiler, '-nostdlib', fixture, 'main.cpp', '-o', 'program'], scope, env)
        run(['./program'], scope, env)


if __name__ == '__main__':
    main()
