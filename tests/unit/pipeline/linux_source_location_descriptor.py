"""Run the pinned source_location consumer as an x86-64 ELF object (#327)."""

import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run


def main():
    ycxx, host, libycxx_source, source = sys.argv[1:]
    libycxx = Path(libycxx_source)
    includes = ['-isystem', libycxx / 'include']
    marker = libycxx / 'src/runtime/linkage/static.cpp'
    env = dict(os.environ, LC_ALL='C')
    with tempfile.TemporaryDirectory(prefix='ycxx-source-location-') as temporary:
        scope = Path(temporary)
        run([ycxx, 'cxx', '--no-daemon', '--target=x86_64-pc-linux-gnu', '-std=c++26',
             *includes, '-c', source, '-o', 'source-location.o'], scope, env)
        run([host, '-std=c++26', '-pedantic-errors', '-no-pie', *includes,
             'source-location.o', marker, '-o', 'program'], scope, env)
        run(['./program'], scope, env)


if __name__ == '__main__':
    main()
