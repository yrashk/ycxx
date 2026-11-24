"""Link independent ELF section-group fixtures with ycxx's native static linker."""

import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'daemon'))
from runtime_dir import runtime_root


def check_group_payload(image, expected):
    phoff, = struct.unpack_from('<Q', image, 32)
    entsize, count = struct.unpack_from('<HH', image, 54)
    headers = (struct.unpack_from('<II6Q', image, phoff + i * entsize) for i in range(count))
    writable, = (h for h in headers if h[0:2] == (1, 6))  # writable PT_LOAD
    offset, _, _, file_size = writable[2:6]
    assert file_size == 12, 'the executable must contain exactly one grouped data payload'
    assert struct.unpack_from('<I', image, offset + 8)[0] == expected


def main():
    ycxx, cc, ar = sys.argv[1:]
    fixtures = Path(__file__).resolve().with_suffix('')
    target = '--target=x86_64-pc-linux-gnu'
    with tempfile.TemporaryDirectory(prefix='yccomdat-') as temp, \
            tempfile.TemporaryDirectory(prefix='yccomdat-rt-', dir=runtime_root('yccomdat-rt-')) as runtime:
        scope = Path(temp).resolve()
        (scope / '.ycxx').touch()
        base = {k: v for k, v in os.environ.items()
                if k not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT', 'YCXX_NO_WATCH')}
        base.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='4', XDG_CACHE_HOME=str(scope / 'cache'))
        cold = dict(base, YCXX_NO_DAEMON='1')
        daemon = dict(base, YCXX_DAEMON='require')

        def run(args, status=0, error='', env=cold):
            result = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=True, timeout=60)
            assert result.returncode == status and error in result.stderr, (args, result, status, error)
            if not error:
                assert not result.stderr, (args, result)
            return result.stdout

        def link(*inputs, result, env=cold):
            run([ycxx, 'cxx', target, '-nostdlib', *inputs, '-o', 'program'], env=env)
            assert not run(['./program'], status=result)
            return (scope / 'program').read_bytes()

        def reject(*inputs, error, env=cold):
            run([ycxx, 'cxx', target, '-nostdlib', *inputs, '-o', 'rejected'], status=1, error=error, env=env)
            assert not (scope / 'rejected').exists()

        def daemon_pid():
            status = run([ycxx, 'ctl', 'status'], env=base)
            pid, = (line.removeprefix('pid: ') for line in status.splitlines() if line.startswith('pid: '))
            return int(pid)

        for name in ('group-a', 'group-b', 'group-start', 'strong-a', 'strong-b', 'group-b-ghost',
                     'group-b-discarded-export', 'group-demand-export-start',
                     'startup-group-winning', 'startup-group-losing'):
            run([cc, '-c', fixtures / f'{name}.S', '-o', f'{name}.o'])

        ordered = ('group-start.o', 'group-a.o', 'group-b.o')
        reversed_groups = ('group-start.o', 'group-b.o', 'group-a.o')
        first = link(*ordered, result=23)
        second = link(*reversed_groups, result=58)
        check_group_payload(first, 23)
        check_group_payload(second, 58)
        assert first != second
        link('strong-a.o', result=0)
        reject('strong-a.o', 'strong-b.o', error="duplicate symbol 'ordinary_duplicate'")
        main = fixtures / 'startup-main.cpp'
        link(main, 'startup-group-winning.o', 'startup-group-losing.o', result=23)
        link(main, 'startup-group-losing.o', 'startup-group-winning.o', result=99)
        assert link('group-start.o', 'group-a.o', 'group-b-ghost.o', result=23) == first
        reject('group-start.o', 'group-b-ghost.o', 'group-a.o', error="undefined symbol 'ghost'")
        start = 'group-demand-export-start.o'
        export = 'group-b-discarded-export.o'
        link(start, export, 'group-a.o', result=91)
        reject(start, 'group-a.o', export, error="undefined symbol 'discarded_export'")

        run([sys.executable, fixtures / 'invalid-member-index.py', 'group-a.o', 'invalid-member.o'])
        reject('group-start.o', 'group-b.o', 'invalid-member.o', error='invalid ELF group member')

        run([ar, 'rcs', 'grouped.a', 'group-a.o'])
        run([ar, 'rcs', 'empty.a'])
        link('strong-a.o', 'grouped.a', result=0)
        reject('group-start.o', 'grouped.a', error='grouped.a(group-a.o): ELF COMDAT groups are unsupported in archive links')
        reject('group-a.o', 'empty.a', error='ELF COMDAT groups are unsupported in archive links')
        archive = (scope / 'empty.a').read_bytes()
        run([ycxx, 'archive', '-o', 'empty.a', 'group-a.o'], status=1,
            error='ELF COMDAT groups are unsupported in native archive production')
        assert (scope / 'empty.a').read_bytes() == archive

        run([cc, '-shared', '-nostdlib', '-Wl,-soname,libpeer.so', 'strong-b.o', '-o', 'libpeer.so'])
        reject('group-a.o', 'libpeer.so', '--dynamic-linker=/elf-comdat-test/loader',
               error='ELF COMDAT groups are unsupported in dynamic links')

        original = (scope / 'group-b.o').read_bytes()
        run([sys.executable, fixtures / 'change-group-signature.py', 'group-b.o', 'new-signature.o'])
        duplicate = "duplicate symbol '_Z12select_groupv'"
        try:
            assert link(*ordered, result=23, env=daemon) == first
            pid = daemon_pid()
            (scope / 'new-signature.o').replace(scope / 'group-b.o')
            reject(*ordered, error=duplicate)
            reject(*ordered, error=duplicate, env=daemon)
            (scope / 'restored.o').write_bytes(original)
            (scope / 'restored.o').replace(scope / 'group-b.o')
            assert link(*ordered, result=23, env=daemon) == link(*ordered, result=23) == first
            assert daemon_pid() == pid
        finally:
            run([ycxx, 'ctl', 'shutdown'], env=base)


if __name__ == '__main__':
    main()
