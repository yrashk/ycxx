"""The Linux x86-64 virtual destructor profile requires ordinary pointer-field layout."""

from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory


def main(ycxx, native):
    fixture = Path(__file__).resolve().with_suffix('.cpp')
    with TemporaryDirectory(prefix='ycxx-derived-dtor-alignment-') as directory:
        scope = Path(directory)
        shutil.copyfile(fixture, scope / fixture.name)
        result = subprocess.run(
            [native, '-std=c++26', '-pedantic-errors', '-DVERIFY_NATIVE_LAYOUT',
             '-c', fixture.name, '-o', 'native.o'],
            cwd=scope, capture_output=True, timeout=30,
        )
        assert result.returncode == 0 and (scope / 'native.o').is_file(), result

        # The native assertions establish the layout of this source on the target.
        # With those assertions absent, the object gate must itself decline the type.
        result = subprocess.run(
            [ycxx, 'cxx', '--no-daemon', '--target=x86_64-pc-linux-gnu', '-std=c++26',
             '-c', fixture.name, '-o', 'ycxx.o'],
            cwd=scope, capture_output=True, timeout=30,
        )
        assert result.returncode == 1 and b'error:' in result.stderr, result
        assert not (scope / 'ycxx.o').exists(), result


if __name__ == '__main__':
    main(*sys.argv[1:])
