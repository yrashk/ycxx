"""Check target and resource queries outside any project (#970)."""
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile


LINUX = 'x86_64-pc-linux-gnu'
DARWIN = 'arm64-apple-darwin'
DUMP_MACHINE = '-dumpmachine'
PRINT_INCLUDE = '-print-file-name=include'


def main():
    ycxx, = sys.argv[1:]
    ycxx = Path(ycxx).resolve()
    with tempfile.TemporaryDirectory(prefix='ycxx-driver-queries-') as temp:
        root = Path(temp)
        scope = root / 'empty'
        scope.mkdir()
        blocked = root / 'blocked-runtime'
        blocked.touch()
        env = dict(os.environ, LC_ALL='C', SDKROOT='', XDG_CACHE_HOME=str(root / 'cache'),
                   YCXX_DAEMON='require', YCXX_RUNTIME_DIR=str(blocked))
        env.pop('YCXX_NO_DAEMON', None)
        env.pop('YCXX_ROOT', None)

        def query(*args):
            result = subprocess.run([ycxx, 'cxx', *args], cwd=scope, env=env,
                                    capture_output=True, text=True, timeout=30)
            lines = result.stdout.splitlines()
            assert result.returncode == 0 and not result.stderr and len(lines) == 1 and lines[0], (args, result)
            assert not list(scope.iterdir()), (args, list(scope.iterdir()))
            return lines[0]

        def resource(*args):
            return (scope / query(PRINT_INCLUDE, *args)).resolve()

        ignored = ('missing.cpp', '-o', 'unexpected.o')
        directories = {}
        for triple, flags, header in (
            (LINUX, (f'--target={LINUX}',), 'stddef.h'),
            (DARWIN, ('--target', DARWIN), 'stdarg.h'),
        ):
            assert query(*flags, DUMP_MACHINE) == triple
            directory = resource(*flags)
            assert directory.is_dir() and (directory / header).is_file(), (triple, directory, header)
            directories[triple] = directory
            assert query(*flags, DUMP_MACHINE, *ignored) == triple
            assert resource(*flags, *ignored) == directory
            for options in ((DUMP_MACHINE, PRINT_INCLUDE), (PRINT_INCLUDE, DUMP_MACHINE)):
                assert query(*options, *flags) == triple
                assert query(*options, *ignored, *flags) == triple

        assert query('--target', 'x86_64-linux-gnu', DUMP_MACHINE) == LINUX
        assert directories[LINUX] != directories[DARWIN], directories
        # PARITY: by design: these build hosts have configured native defaults.
        machine = platform.machine().lower()
        if sys.platform == 'linux' and machine in ('x86_64', 'amd64'):
            assert query(DUMP_MACHINE) == LINUX and resource() == directories[LINUX]
        if sys.platform == 'darwin' and machine in ('arm64', 'aarch64'):
            assert query(DUMP_MACHINE) == DARWIN and resource() == directories[DARWIN]

        assert set(root.iterdir()) == {scope, blocked}, list(root.iterdir())


if __name__ == '__main__':
    main()
