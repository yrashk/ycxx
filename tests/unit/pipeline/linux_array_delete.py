"""Release host-created, cookie-free scalar arrays through ycxx delete expressions."""

from collections import Counter
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from linux_cleanup_free_unwind import object_frames
from linux_compile import run


def check_relocations(readelf, obj, scope, env, unsized, observer):
    metadata = run([readelf, '--wide', '--relocs', obj], scope, env)
    symbols = Counter(fields[4] for fields in map(str.split, metadata.splitlines())
                      if len(fields) >= 5 and fields[2].startswith('R_X86_64_'))
    assert symbols['_ZdaPv'] == unsized, metadata
    assert symbols['observe_array_operand'] == observer, metadata
    assert not any(symbol != '_ZdaPv' and symbol.startswith(('_ZdaPv', '_ZdlPv'))
                   for symbol in symbols), metadata


def main():
    ycxx, host_cxx, readelf = sys.argv[1:]
    fixtures = Path(__file__).resolve().with_suffix('')
    linux = '--target=x86_64-pc-linux-gnu'
    darwin = '--target=arm64-apple-darwin'
    env = {key: value for key, value in os.environ.items() if key not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', SDKROOT='', YCXX_NO_DAEMON='1', YCXX_THREADS='1')

    with tempfile.TemporaryDirectory(prefix='ycxx-array-delete-') as temp:
        scope = Path(temp)
        (scope / '.ycxx').touch()

        def hosted_probe(provider, objects, *arguments):
            host_object = f'{provider}.o'
            binary = f'{provider}-probe'
            run([host_cxx, '-std=c++26', '-pedantic-errors', '-O0', '-fexceptions', '-fno-lto', '-c',
                 fixtures / f'{provider}.cpp', '-o', host_object], scope, env)
            run([host_cxx, '-fno-lto', host_object, *objects, '-o', binary], scope, env)
            print(run([f'./{binary}', *arguments], scope, env), end='')

        objects = []
        for name, unsized, observer in (('release_arrays', 2, 0), ('release_observed_array', 1, 1)):
            source = fixtures / f'{name}.cpp'
            obj = f'{name}.o'
            objects.append(obj)
            run([ycxx, 'cxx', linux, '-std=c++26', '-c', source, '-o', obj], scope, env)
            check_relocations(readelf, obj, scope, env, unsized, observer)

            run([ycxx, 'cxx', darwin, '-std=c++26', '-fsyntax-only', source], scope, env)
            rejected = f'{name}-darwin.o'
            command = [ycxx, 'cxx', darwin, '-std=c++26', '-c', source, '-o', rejected]
            result = subprocess.run(command, cwd=scope, env=env, capture_output=True, text=True, timeout=30)
            assert result.returncode == 1, (command, result.returncode, result.stdout, result.stderr)
            assert ': error: function_ir: array deallocation is not supported yet' in result.stderr, result.stderr
            assert not (scope / rejected).exists(), (command, result.stderr)

        hosted_probe('host_provider', objects, '--require-null-skip')

        throwing = 'release_throwing_operand.o'
        run([ycxx, 'cxx', linux, '-std=c++26', '-c', fixtures / 'release_throwing_operand.cpp',
             '-o', throwing], scope, env)
        _, symbols, covered, _ = object_frames(scope / throwing)
        binding, start, size = symbols['release_from_peer']
        assert binding == 1 and covered == [(start, size)], (symbols, covered)
        hosted_probe('host_throwing_operand', [throwing])


if __name__ == '__main__':
    main()
