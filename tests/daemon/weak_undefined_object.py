"""Check the weak object declaration of the pinned allocator marker (#984)."""

from pathlib import Path
import subprocess
import sys

from process import Env, failures
from system_includes import LINUX, NATIVE_LINUX


MARKER = '__ycxx_fs_default_new'
WEAK = b'extern "C" [[__gnu__::__weak__]] const char __ycxx_fs_default_new;'
STRONG = b'extern "C" const char __ycxx_fs_default_new;'


def check_symbol(symbols, relocations, binding):
    marker, = (row.split() for row in symbols.splitlines() if row.split()[-1:] == [MARKER])
    assert marker[4:7] == [binding, 'DEFAULT', 'UND'], (binding, symbols)
    references = [row.split() for row in relocations.splitlines() if MARKER in row.split()]
    got = {'R_X86_64_GOTPCREL', 'R_X86_64_GOTPCRELX', 'R_X86_64_REX_GOTPCRELX'}
    assert any(row[2] in got for row in references), relocations
    return marker[3]


def check(world, libycxx, host, readelf, ar):
    scope = world.scope.resolve()
    fixtures = Path(__file__).resolve().with_suffix('')

    def run(*args, status=0):
        result = subprocess.run(args, cwd=scope, env=world.cold, capture_output=True, text=True, timeout=30)
        assert result.returncode == status, (args, status, result)
        return result.stdout

    for name in ('consumer.cpp', 'caller.cpp', 'provider.cpp'):
        (scope / name).write_bytes((fixtures / name).read_bytes())
    header = scope / 'try_or_null.hpp'
    pinned = libycxx / 'src/freestanding/new' / header.name
    original = pinned.read_bytes()
    assert original.count(WEAK) == 1, pinned
    strong = original.replace(WEAK, STRONG)

    mode = [LINUX, '-std=c++26', '-ffreestanding', '-nostdinc', '-isystem', libycxx / 'include']

    def object_bytes(source, obj):
        args = [*mode, '-c', source, '-o', obj.name]
        results = []
        images = []
        for env in (world.cold, world.daemon):
            obj.unlink(missing_ok=True)
            result = world.cxx(args, scope, env, timeout=30)
            assert result[0] == 0 and obj.is_file(), (args, result)
            results.append(result)
            images.append(obj.read_bytes())
        assert results[0] == results[1] and images[0] == images[1], (args, results)
        return images[0]

    obj = scope / 'consumer.o'
    snapshots = []
    kinds = []
    for contents, binding in ((original, 'WEAK'), (strong, 'GLOBAL'), (original, 'WEAK')):
        header.write_bytes(contents)
        image = object_bytes('consumer.cpp', obj)
        if not snapshots:
            pid = world.pid()
        snapshots.append(image)
        kinds.append(check_symbol(run(readelf, '-Ws', obj), run(readelf, '-Wr', obj), binding))
    assert snapshots[0] == snapshots[2] and snapshots[0] != snapshots[1]
    assert kinds[0] == kinds[1] == kinds[2]
    world.same_daemon('weak declaration edit and restoration', pid)
    (scope / 'strong.o').write_bytes(snapshots[1])

    # The redeclaration has its own top-level item, apart from the earlier function declaration.
    prefix = STRONG + b'\nextern "C" const char* late_address();\n'
    suffix = b'\nextern "C" const char* late_address() { return &__ycxx_fs_default_new; }\n'
    late_weak = STRONG.replace(b'const', b'[[gnu::weak]] const', 1)
    source = scope / 'late.cpp'
    obj = scope / 'late.o'
    snapshots = []
    for declaration, binding in ((STRONG, 'GLOBAL'), (late_weak, 'WEAK'), (STRONG, 'GLOBAL')):
        source.write_bytes(prefix + declaration + suffix)
        snapshots.append(object_bytes(source.name, obj))
        check_symbol(run(readelf, '-Ws', obj), run(readelf, '-Wr', obj), binding)
    assert snapshots[0] == snapshots[2] and snapshots[0] != snapshots[1]
    world.same_daemon('late weak declaration edit and restoration', pid)

    # The exact pinned header's static linkage guard has its own provider.
    marker = libycxx / 'src/runtime/linkage/static.cpp'
    args = [*mode, '-c', marker, '-o', 'linkage.o']
    result = world.cxx(args, scope, world.cold, timeout=30)
    assert result[0] == 0, (args, result)
    for name in ('caller', 'provider'):
        run(host, '-std=c++26', '-pedantic-errors', '-fPIE', '-c', f'{name}.cpp', '-o', f'{name}.o')
    run(ar, 'rcs', 'provider.a', 'provider.o')

    program = scope / 'program'
    for inputs, status in ((('consumer.o',), 0), (('consumer.o', 'provider.a'), 0),
                           (('consumer.o', 'provider.o'), 41), (('strong.o', 'provider.a'), 41)):
        program.unlink(missing_ok=True)
        run(host, '-pie', 'caller.o', 'linkage.o', *inputs, '-o', program)
        run(program, *(['provided'] if status else []), status=status)
    program.unlink(missing_ok=True)
    args = [host, '-pie', 'caller.o', 'linkage.o', 'strong.o', '-o', program]
    result = subprocess.run(args, cwd=scope, env=world.cold, capture_output=True, text=True, timeout=30)
    assert result.returncode != 0 and 'undefined' in result.stderr and MARKER in result.stderr, (args, result)
    assert not program.exists(), program


def main():
    # PARITY: by design: these symbol and archive checks use an x86-64 Linux ELF linker.
    if not NATIVE_LINUX:
        return 0
    ycxx, libycxx = (Path(arg).resolve() for arg in sys.argv[1:3])
    world = Env(str(ycxx))
    for env in (world.base, world.cold, world.daemon):
        env.update(LC_ALL='C', YCXX_THREADS='2')
    try:
        check(world, libycxx, *sys.argv[3:])
    finally:
        world.close()
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
