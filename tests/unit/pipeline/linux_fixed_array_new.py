"""Allocate fixed-bound scalar arrays and unwind to a hosted C++ caller."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from linux_cleanup_free_unwind import object_frames
from linux_compile import run


def check_object(readelf, obj, scope, env):
    expected = {
        'make_int_array': '_Znam',
        'make_pointer_array': '_Znam',
        'make_empty_int_array': '_Znam',
        'release_int_array': '_ZdaPv',
        'release_pointer_array': '_ZdaPv',
    }
    _, functions, covered, _ = object_frames(scope / obj)
    assert functions.keys() == expected.keys(), functions
    assert all(binding == 1 for binding, _, _ in functions.values()), functions
    assert set(covered) == {(start, size) for _, start, size in functions.values()}, (functions, covered)

    metadata = run([readelf, '--wide', '--relocs', obj], scope, env)
    relocations = [(int(fields[0], 16), fields[2], fields[4])
                   for fields in map(str.split, metadata.splitlines())
                   if len(fields) >= 5 and fields[2].startswith('R_X86_64_')
                   and fields[4].startswith(('_Zn', '_Zd'))]
    assert len(relocations) == len(expected), metadata
    for name, symbol in expected.items():
        _, start, size = functions[name]
        actual = [(kind, callee) for at, kind, callee in relocations if start <= at < start + size]
        assert actual == [('R_X86_64_PLT32', symbol)], (name, actual, metadata)


def main():
    ycxx, host_cxx, readelf = sys.argv[1:]
    fixtures = Path(__file__).resolve().with_suffix('')
    provider = fixtures / 'host_fixed_array_provider.cpp'
    flags = [ycxx, 'cxx', '-std=c++26']
    linux = '--target=x86_64-pc-linux-gnu'
    darwin = '--target=arm64-apple-darwin'
    env = {key: value for key, value in os.environ.items() if key not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', SDKROOT='', YCXX_NO_DAEMON='1', YCXX_THREADS='1')

    with tempfile.TemporaryDirectory(prefix='ycxx-fixed-array-new-') as temp:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        run([host_cxx, '-std=c++26', '-pedantic-errors', '-O0', '-fexceptions', '-fno-lto', '-c',
             provider, '-o', 'provider.o'], scope, env)

        for name in ('fixed_array_wrappers', 'computed_literal_array_wrappers'):
            wrappers = fixtures / f'{name}.cpp'
            obj = f'{name}.o'
            run([*flags, linux, '-c', wrappers, '-o', obj], scope, env)
            check_object(readelf, obj, scope, env)

            run([*flags, darwin, '-fsyntax-only', wrappers], scope, env)
            rejected = f'{name}-darwin.o'
            command = [*flags, darwin, '-c', wrappers, '-o', rejected]
            result = subprocess.run(command, cwd=scope, env=env, capture_output=True, text=True, timeout=30)
            assert result.returncode == 1, (command, result.returncode, result.stdout, result.stderr)
            assert ': error: function_ir: array allocation is not supported yet' in result.stderr, result.stderr
            assert not (scope / rejected).exists(), (command, result.stderr)

            probe = f'{name}-probe'
            run([host_cxx, '-fno-lto', 'provider.o', obj, '-o', probe], scope, env)
            print(wrappers.name)
            print(run([f'./{probe}'], scope, env), end='')


if __name__ == '__main__':
    main()
