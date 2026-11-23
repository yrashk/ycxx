"""A typedef's alignment must reach the by-value parameter ABI check (#992)."""

from pathlib import Path
import sys

from process import Env, bytes_of, failures
from system_includes import LINUX


PLAIN = '''\
struct S { long a, b; };
typedef S Aligned;
long use(Aligned s) { return s.a; }
'''
ALIGNED = PLAIN.replace('typedef S Aligned;', 'typedef S Aligned __attribute__((aligned(32)));')
ABI_ERROR = b'error: x86-64 System V ABI: unsupported aggregate parameter layout'


def compile_state(world, expected_error=None):
    output = world.scope / 'aligned.o'
    args = [LINUX, '-std=c++26', '-c', 'aligned.cpp', '-o', output.name]
    results = []
    for env in (world.cold, world.daemon):
        output.unlink(missing_ok=True)
        result = world.cxx(args, world.scope, env, timeout=30)
        results.append((result, bytes_of(output)))

    cold, warm = results
    assert warm[0] == cold[0], (expected_error, cold[0], warm[0])
    assert warm[1] == cold[1], 'daemon output differs from a cold build'
    result, obj = cold
    if expected_error is None:
        assert result == (0, b'', b''), result
        assert obj and obj.startswith(b'\x7fELF'), 'expected an ELF object'
    else:
        assert result[0] == 1 and any(line.endswith(expected_error) for line in result[2].splitlines()), result
        assert obj is None, 'a rejected compile wrote an object'
    return obj


def main():
    world = Env(str(Path(sys.argv[1]).resolve()))
    for env in (world.base, world.cold, world.daemon):
        env.update(LC_ALL='C', YCXX_THREADS='2')
    try:
        source = world.scope / 'aligned.cpp'
        # Each declaration is a separate group. Only the typedef changes across the edits.
        source.write_text(PLAIN, encoding='utf-8')
        original = compile_state(world)
        pid = world.pid()
        source.write_text(ALIGNED, encoding='utf-8')
        compile_state(world, ABI_ERROR)
        world.same_daemon('added typedef alignment', pid)
        source.write_text(PLAIN, encoding='utf-8')
        restored = compile_state(world)
        world.same_daemon('removed typedef alignment', pid)
        assert restored == original, 'restoring the typedef changed the object'
    finally:
        world.close()
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
