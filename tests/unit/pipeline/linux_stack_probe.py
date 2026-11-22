"""Probe x86-64 frames larger than a page so they cannot skip a guard page (#644)."""

import os
from pathlib import Path
import shutil
import sys
import tempfile

from linux_compile import run


def main():
    ycxx, cc = sys.argv[1:]
    sources = Path(__file__).resolve().parent / 'linux_stack_probe'
    with tempfile.TemporaryDirectory(prefix='ycxx-probe-') as temp:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        for source in sources.iterdir():
            shutil.copyfile(source, scope / source.name)
        env = dict(os.environ, LC_ALL='C', YCXX_NO_DAEMON='1')
        run([ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-c', 'frames.cpp', '-o', 'frames.o'], scope, env)
        run([cc, '-O1', 'host.c', 'frames.o', '-pthread', '-o', 'probe'], scope, env)
        run(['./probe'], scope, env)


if __name__ == '__main__':
    main()
