"""Verify native GNU archive production and linking from an archived ELF object."""

import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

from linux_archive import symbol_attributes


def member(field, payload, mode='0'):
    header = field.encode('utf-8').ljust(16) + f'0           0     0     {mode:8}{len(payload):<10}`\n'.encode()
    assert len(header) == 60
    return header + payload + b'\n' * (len(payload) % 2)


def main():
    ycxx, ar, nm, ld, *compilers = sys.argv[1:]
    cc = compilers[0] if compilers else 'cc'
    target = '--target=x86_64-pc-linux-gnu'
    magic = b'!<arch>\n'
    with tempfile.TemporaryDirectory(prefix='ycxx-archive-output-') as temp:
        scope = Path(temp)
        base = dict(os.environ, LC_ALL='C', YCXX_THREADS='2')
        cold = dict(base, YCXX_NO_DAEMON='1')
        offline = dict(base, PATH='', YCXX_DAEMON='require')
        offline.pop('YCXX_NO_DAEMON', None)

        def run(args, status=0, error=b'', env=cold):
            p = subprocess.run(args, cwd=scope, env=env, capture_output=True, timeout=30)
            if p.returncode != status or error not in p.stderr:
                raise AssertionError(f'{args} exited {p.returncode}, expected {status} and {error!r}\n'
                                     f'{p.stdout!r}\n{p.stderr!r}')
            return p.stdout

        def archive(*args, **kwargs):
            return run([ycxx, 'archive', *args], env=offline, **kwargs)

        def object_file(name, out, contents):
            (scope / name).write_text(contents + '\n')
            run([ycxx, 'cxx', target, '-c', name, '-o', out])

        archive('-o', 'empty.a')
        assert b'archive' in run([ycxx, '--help'])
        assert b'ycxx archive -o' in archive('--help')
        empty = magic + member('/', b'\0' * 4)
        assert (scope / 'empty.a').read_bytes() == empty

        for directory in ('first', 'second'):
            (scope / directory).mkdir()
        short_name = '1234567890123.o'  # 15 bytes, plus the short-name slash.
        tail_name = 'abcdefghijklmnop'  # 16 bytes and no object suffix.
        head_name = 'name space.o'
        reserved = '__.SYMDEF'
        paths = [tail_name, f'first/{short_name}', head_name, f'second/{short_name}', reserved, '-local.o']
        object_file('tail.cpp', paths[0], 'extern "C" int tail() { return 37; }')
        symbol_attributes(scope / paths[0], 'tail', weak=True)
        object_file('first.cpp', paths[1], 'extern "C" int repeated() { return 2; }')
        object_file('head.cpp', paths[2], 'extern "C" int tail(); static int helper() { return tail(); }'
                    ' extern "C" int headx() { return helper(); }')
        symbol_attributes(scope / paths[2], 'headx', hidden=True)
        # ELF permits trailing bytes outside its sections. Keep this byte and align the archive member.
        with (scope / paths[2]).open('ab') as f:
            f.write(b'\x80')
        object_file('second.cpp', paths[3], 'extern "C" int repeated() { return 89; }')
        object_file('local.cpp', paths[4], 'static int private_only() { return 5; }')
        shutil.copyfile(scope / paths[4], scope / paths[5])
        unicode_name = 'λ.o'
        shutil.copyfile(scope / paths[4], scope / unicode_name)

        archive('-o', 'output.a', '--', *paths)
        output = (scope / 'output.a').read_bytes()
        payloads = [(scope / p).read_bytes() for p in paths]
        assert len(payloads[2]) % 2 == 1
        long_names = (tail_name + '/\n' + head_name + '/\n' + reserved + '/\n').encode('ascii')
        assert len(long_names) % 2 == 1
        table = member('//', long_names)
        fields = ['/0', short_name + '/', f'/{len(tail_name) + 2}', short_name + '/',
                  f'/{len(tail_name) + len(head_name) + 4}', '-local.o/']
        ordinary = [member(field, data, '644') for field, data in zip(fields, payloads, strict=True)]
        symbols = b'tail\0repeated\0headx\0repeated\0'
        assert len(symbols) % 2 == 1
        first = len(magic) + len(member('/', b'\0' * 20 + symbols)) + len(table)
        offsets = [first + sum(map(len, ordinary[:i])) for i in range(4)]
        index = member('/', struct.pack('>5I', 4, *offsets) + symbols)
        assert output == magic + index + table + b''.join(ordinary)

        names = [Path(p).name for p in paths]
        for tool in filter(None, (ar, shutil.which('llvm-ar'))):
            assert run([tool, 't', 'empty.a']) == b''
            assert run([tool, 't', 'output.a']).decode().splitlines() == names
            for i in (0, 1, 2, 4):
                assert run([tool, 'p', 'output.a', names[i]]) == payloads[i]
        armapped = [f'{s} in {names[i]}' for i, s in enumerate(('tail', 'repeated', 'headx', 'repeated'))]
        for tool in filter(None, (nm, shutil.which('llvm-nm'))):
            assert run([tool, '--print-armap', 'empty.a']) == b''
            lines = run([tool, '--print-armap', 'output.a']).decode().splitlines()
            assert [line for line in lines if ' in ' in line] == armapped

        archive('-o', 'local.a', '--', '-local.o', unicode_name)
        assert (scope / 'local.a').read_bytes() == empty + ordinary[-1] + member(unicode_name + '/', payloads[-1], '644')
        for i, path in enumerate(paths):
            os.chmod(scope / path, 0o600 if i % 2 else 0o755)
            os.utime(scope / path, ns=(i * 10**11, (i + 1) * 10**12))
        archive('-o', 'deterministic.a', '--', *paths)
        assert (scope / 'deterministic.a').read_bytes() == output

        object_file('main.cpp', 'main.o', 'extern "C" int headx(); extern "C" int repeated();'
                    ' int main() { return headx() + repeated(); }')
        run([ycxx, 'cxx', target, '-nostdlib', 'main.o', 'output.a', '-o', 'native'])
        run(['./native'], status=39)
        (scope / 'entry.s').write_text('''
.text
.globl _start
_start:
  call main
  mov %eax, %edi
  mov $60, %eax
  syscall
.section .note.GNU-stack,"",@progbits
''')
        run([cc, '-c', 'entry.s', '-o', 'entry.o'])
        for linker in filter(None, (ld, shutil.which('ld.lld'))):
            run([linker, '-o', 'external', 'entry.o', 'main.o', 'output.a'])
            run(['./external'], status=39)

        (scope / 'bad.o').write_bytes(b'truncated')
        wrong_target = bytearray(payloads[0])
        struct.pack_into('<H', wrong_target, 18, 183)  # EM_AARCH64
        (scope / 'wrong-target.o').write_bytes(wrong_target)
        (scope / 'new\nline.o').write_bytes(payloads[0])
        with (scope / 'oversized.o').open('wb') as f:
            f.truncate(10_000_000_000)  # Sparse; the member's size field has 10 digits.
        for part, data in (('defined', payloads[0]), ('undefined', payloads[2])):
            path = scope / f'unnamed-{part}.o'
            path.write_bytes(data)
            symbol_attributes(path, 'tail', unnamed=True)
        for later, error in (('bad.o', b'truncated ELF header'),
                             ('unnamed-defined.o', b'unnamed-defined.o: empty name for nonlocal ELF symbol'),
                             ('unnamed-undefined.o', b'unnamed-undefined.o: empty name for nonlocal ELF symbol'),
                             ('wrong-target.o', b'expected an x86-64 relocatable ELF object'),
                             ('new\nline.o', b'cannot represent member basename'),
                             ('oversized.o', b'archive member size exceeds 10 decimal digits'),
                             ('missing.o', b'cannot read')):
            archive('-o', 'output.a', paths[0], later, status=1, error=error)
            assert (scope / 'output.a').read_bytes() == output
        archive('-o', 'absent.a', paths[0], 'bad.o', status=1, error=b'truncated ELF header')
        assert not (scope / 'absent.a').exists()
        archive('-o', 'output.a', '-local.o', status=2, error=b'unknown option')
        assert (scope / 'output.a').read_bytes() == output
        archive('-o', 'output.a')
        assert (scope / 'output.a').read_bytes() == empty
        assert not list(scope.glob('.ycxx-archive-*'))


if __name__ == '__main__':
    main()
