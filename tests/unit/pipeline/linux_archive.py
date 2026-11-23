"""Exercise ordered Linux archive extraction through the CLI and the resident daemon."""

import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def archive_member(name, data):
    """Make a container member for malformed and unsupported format fixtures."""
    header = f'{name:16}{0:<12}{0:<6}{0:<6}{644:<8}{len(data):<10}`\n'
    assert len(header) == 60
    return header.encode('ascii') + data + (b'\n' if len(data) % 2 else b'')


def symbol_attributes(path, symbol, *, weak=False, hidden=False, unnamed=False):
    """Change attributes on an existing ELF symbol."""
    data = bytearray(path.read_bytes())
    shoff = struct.unpack_from('<Q', data, 40)[0]
    shentsize, shnum = struct.unpack_from('<HH', data, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    symtab, = (s for s in sections if s[1] == 2)  # SHT_SYMTAB
    names = sections[symtab[6]]
    strings = data[names[4]:names[4] + names[5]]
    matches = []
    for entry in range(symtab[4], symtab[4] + symtab[5], symtab[9]):
        name_offset = struct.unpack_from('<I', data, entry)[0]
        if strings[name_offset:strings.index(0, name_offset)] == symbol.encode('ascii'):
            matches.append(entry)
    entry, = matches
    if weak:
        assert data[entry + 4] >> 4 == 1  # STB_GLOBAL
        data[entry + 4] = (data[entry + 4] & 15) | (2 << 4)  # STB_WEAK
    if hidden:
        data[entry + 5] = 2  # STV_HIDDEN
    if unnamed:
        struct.pack_into('<I', data, entry, 0)  # st_name points to the null string
    path.write_bytes(data)


def main():
    ycxx, ar = sys.argv[1:]
    target = '--target=x86_64-pc-linux-gnu'
    magic = b'!<arch>\n'
    with tempfile.TemporaryDirectory(prefix='ycxx-linux-archive-', dir='.') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temp).resolve()
        (scope / '.ycxx').touch()
        base = {k: v for k, v in os.environ.items() if k not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT')}
        base.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')
        cold = dict(base, YCXX_NO_DAEMON='1')
        daemon = dict(base, YCXX_DAEMON='require')

        def run(args, status=0, error='', env=cold):
            result = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=True, timeout=45)
            if result.returncode != status or error not in result.stderr:
                raise AssertionError(f'{args} exited {result.returncode}, expected {status}'
                                     f' and stderr containing {error!r}\n{result.stdout}{result.stderr}')
            return result.stdout

        def source(name, text, out=None):
            (scope / name).write_text(text + '\n')
            if out is not None:
                run([ycxx, 'cxx', target, '-c', name, '-o', out])

        def archive(out, *members, flags='rcs'):
            run([ar, flags, out, *members])

        def link(*inputs, result, env=cold):
            run([ycxx, 'cxx', target, '-nostdlib', *inputs, '-o', 'program'], env=env)
            run(['./program'], status=result)
            return (scope / 'program').read_bytes()

        def reject(*args, error, flags=(target, '-nostdlib'), env=cold):
            run([ycxx, 'cxx', *flags, *args, '-o', 'rejected'], status=1, error=error, env=env)
            assert not (scope / 'rejected').exists()

        def daemon_pid():
            status = run([ycxx, 'ctl', 'status'], env=base)
            pid, = (line.removeprefix('pid: ') for line in status.splitlines() if line.startswith('pid: '))
            return int(pid)

        source('main.cpp', 'extern "C" int head(); int main() { return head(); }', 'main.o')
        source('head.cpp', 'extern "C" int tail(); extern "C" int head() { return tail(); }',
               'selected_member_with_a_long_name.o')
        source('tail.cpp', 'extern "C" int tail() { return 37; }', 'tail.o')
        (scope / 'unselected_data_with_a_long_name.bin').write_bytes(b'odd')
        archive('chain.a', 'tail.o', 'selected_member_with_a_long_name.o', 'unselected_data_with_a_long_name.bin')
        assert run([ar, 't', 'chain.a']).splitlines() == [
            'tail.o', 'selected_member_with_a_long_name.o', 'unselected_data_with_a_long_name.bin']
        image = link('main.cpp', 'chain.a', result=37, env=dict(cold, PATH=''))
        assert link('main.o', 'chain.a', result=37) == image
        assert link('chain.a', 'main.cpp', 'chain.a', result=37) == image
        reject('chain.a', 'main.cpp', error="undefined symbol 'head'")
        reject('chain.a', 'main.o', error="undefined symbol 'head'")

        archive('head.a', 'selected_member_with_a_long_name.o')
        archive('tail.a', 'tail.o')
        assert link('main.o', 'head.a', 'tail.a', result=37) == image
        reject('main.o', 'tail.a', 'head.a', error="undefined symbol 'tail'")
        assert link('main.o', 'head.a', 'tail.cpp', 'tail.a', result=37) == image

        source('order-state.cpp', 'static int order;'
               ' extern "C" int push(int digit) { order = order * 4 + digit; return order; }'
               ' extern "C" int current() { return order; }', 'order-state.o')
        source('order-disk.cpp', 'extern "C" int push(int); extern "C" int current(); extern "C" int choose();'
               ' static int one = push(1);'
               ' int main() { choose(); return current(); }', 'order-disk.o')
        source('order-member.cpp', 'extern "C" int push(int); static int two = push(2);'
               ' extern "C" int choose() { return two; }', 'order-member.o')
        source('order-source.cpp', 'extern "C" int push(int); static int three = push(3);')
        archive('ordered.a', 'order-member.o')
        link('order-state.o', 'order-disk.o', 'ordered.a', 'order-source.cpp', result=27)  # 123 in base 4
        link('order-state.o', 'order-source.cpp', 'order-disk.o', 'ordered.a', result=54)  # 312 in base 4

        source('trivial.cpp', 'int main() { return 7; }')
        archive('empty.a')
        assert link('trivial.cpp', 'empty.a', result=7) == link('trivial.cpp', result=7)
        reject('chain.a', error='at least one source or ordinary object')
        reject('main.cpp', 'chain.a', flags=(target,), error='hosted Linux linking is not supported')
        reject('main.cpp', 'chain.a', flags=('--target=arm64-apple-darwin',),
               error="chain.a: GNU archive indexes and long names are unsupported in Darwin links")
        reject('-c', 'main.cpp', 'chain.a', error='archive inputs require an executable link')
        reject('main.cpp', 'chain.a', '--dynamic-linker=/some/loader', error='archive inputs cannot be combined')
        reject('main.cpp', 'chain.a', 'provider.so', error='archive inputs cannot be combined')
        reject('main.cpp', 'absent.a', error="cannot read 'absent.a'")

        # Two members can have the same name. Selection follows physical order even if the
        # index presents the later member first, and includes all definitions of a member.
        for d in ('first', 'second'):
            (scope / d).mkdir()
        source('first.cpp', 'extern "C" int head() { return 13; }', 'first/same.o')
        source('second.cpp', 'extern "C" int head() { return 89; } extern "C" int extra() { return 1; }',
               'second/same.o')
        archive('duplicate.a', 'first/same.o', 'second/same.o', flags='qcs')
        assert run([ar, 't', 'duplicate.a']).splitlines() == ['same.o', 'same.o']
        duplicate = bytearray((scope / 'duplicate.a').read_bytes())
        count = struct.unpack_from('>I', duplicate, 68)[0]
        assert count == 3
        index_names = duplicate[72 + 4 * count:].split(b'\0', count)
        heads = [i for i, name in enumerate(index_names[:count]) if name == b'head']
        earlier, later = sorted(struct.unpack_from('>I', duplicate, 72 + 4 * i)[0] for i in heads)
        struct.pack_into('>I', duplicate, 72 + 4 * heads[0], later)
        struct.pack_into('>I', duplicate, 72 + 4 * heads[1], earlier)
        (scope / 'duplicate.a').write_bytes(duplicate)
        link('main.cpp', 'duplicate.a', result=13)
        source('duplicate-main.cpp', 'extern "C" int head(); extern "C" int extra();'
               ' int main() { return head() + extra(); }')
        reject('duplicate-main.cpp', 'duplicate.a', error="duplicate.a(same.o): duplicate symbol 'head'")

        source('weak-use.cpp', 'extern "C" int head(); int main(int n, char**) {'
               ' if (n == 999) return head(); return 17; }', 'weak-use.o')
        symbol_attributes(scope / 'weak-use.o', 'head', weak=True)
        assert link('weak-use.o', 'duplicate.a', result=17) == link('weak-use.o', result=17)
        source('weak-def.cpp', 'extern "C" int head() { return 67; }', 'weak-def.o')
        symbol_attributes(scope / 'weak-def.o', 'head', weak=True)
        archive('weak.a', 'weak-def.o')
        link('main.cpp', 'weak.a', result=67)
        link('main.cpp', 'weak.a', 'duplicate.a', result=67)
        link('main.cpp', 'weak.a', 'first/same.o', result=13)
        symbol_attributes(scope / 'main.o', 'head', hidden=True)
        symbol_attributes(scope / 'first/same.o', 'head', hidden=True)
        archive('hidden.a', 'first/same.o')
        link('main.o', 'hidden.a', result=13)

        source('anchor.cpp', 'int alias() asm("main"); int anchor() { return alias(); }', 'anchor.o')
        source('entry.cpp', 'int main() { return 59; }', 'entry.o')
        archive('entry.a', 'entry.o')
        link('anchor.o', 'entry.a', result=59)
        source('bad-entry.cpp', 'long alias() asm("main"); long alias() { return 1; }')
        reject('anchor.o', 'empty.a', 'bad-entry.cpp',
               error="Linux startup requires 'int main()' or 'int main(int, char**)'")
        source('unrelated.cpp', 'int unrelated() { return 0; }')
        reject('unrelated.cpp', 'entry.a', error="ELF entry symbol 'main' is undefined")

        # Mutate well-formed output from the system ar to test malformed bounds and a stale index.
        chain = (scope / 'chain.a').read_bytes()
        count = struct.unpack_from('>I', chain, 68)[0]
        assert count == 2
        index_names = chain[72 + 4 * count:].split(b'\0', count)
        tail_index = index_names.index(b'tail')
        head_index = index_names.index(b'head')
        head_offset = struct.unpack_from('>I', chain, 72 + 4 * head_index)[0]
        tail_offset = struct.unpack_from('>I', chain, 72 + 4 * tail_index)[0]
        bad_offset = bytearray(chain)
        struct.pack_into('>I', bad_offset, 72 + 4 * head_index, head_offset + 60)
        wrong_symbol = bytearray(chain)
        struct.pack_into('>I', wrong_symbol, 72 + 4 * head_index, tail_offset)
        wrong_payload = bytearray(chain)
        wrong_payload[head_offset + 60] = 0
        bad_count = bytearray(chain)
        struct.pack_into('>I', bad_count, 68, 0xffffffff)
        for data, diagnostic in (
            (b'invalid', 'expected a regular GNU archive'),
            (magic + b'part', 'truncated archive member header'),
            (chain[:66] + b'!!' + chain[68:], 'invalid archive member header trailer'),
            (chain[:56] + b'4294967296' + chain[66:], 'archive member contents exceed the file'),
            (chain[:56] + b'1x        ' + chain[66:], 'invalid archive member size'),
            (chain[:-1], 'missing archive member alignment padding'),
            (bad_count, 'invalid archive symbol count'),
            (bad_offset, 'invalid archive index member offset'),
            (wrong_symbol, "broken.a(tail.o): archive index symbol 'head' is not defined by the member"),
            (wrong_payload, 'broken.a(selected_member_with_a_long_name.o): expected a little-endian ELF64 object'),
            (magic + archive_member('name.o/', b'ELF'), 'nonempty archive has no symbol index'),
            (magic + archive_member('/SYM64/', b''), '64-bit archive indexes are unsupported'),
            (magic + archive_member('__.SYMDEF', b''), 'BSD/Darwin archive indexes and inline names are unsupported'),
            (magic + archive_member('#1/16', b''), 'BSD/Darwin archive indexes and inline names are unsupported'),
            (magic + archive_member('/', bytes(4)) + archive_member('/1', b'') +
             archive_member('//', b'name.o/\n'), 'invalid archive long-name table offset'),
            (magic + archive_member('/', bytes(4)) + archive_member('/0', b'') +
             archive_member('//', b'unterminated'), 'invalid archive long-name table entry'),
            (magic + archive_member('/', struct.pack('>II', 1, 78) + b'xx') +
             archive_member('name.o/', b''), 'unterminated archive index symbol'),
            (magic + archive_member('/', bytes(4)) + archive_member('/', bytes(4)), 'duplicate archive symbol index'),
        ):
            (scope / 'broken.a').write_bytes(data)
            reject('main.cpp', 'broken.a', error=diagnostic)
        (scope / 'broken.a').write_bytes(bad_offset)
        reject('trivial.cpp', 'broken.a', error='invalid archive index member offset')

        archive('thin.a', 'tail.o', flags='rcsT')
        reject('main.cpp', 'thin.a', error='thin archives are unsupported')

        try:
            assert link('main.cpp', 'chain.a', result=37, env=daemon) == image
            pid = daemon_pid()
            assert link('main.cpp', 'chain.a', result=37, env=daemon) == image
            source('tail.cpp', 'extern "C" int tail() { return 53; }', 'tail.o')
            archive('replacement.a', 'tail.o', 'selected_member_with_a_long_name.o',
                    'unselected_data_with_a_long_name.bin')
            chain_path = scope / 'chain.a'
            replacement_path = scope / 'replacement.a'
            stamp = chain_path.stat()
            os.utime(replacement_path, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
            replacement_path.replace(chain_path)
            updated = chain_path.stat()
            assert (updated.st_size, updated.st_mtime_ns) == (stamp.st_size, stamp.st_mtime_ns)
            replacement = link('main.cpp', 'chain.a', result=53, env=daemon)
            assert replacement != image and replacement == link('main.cpp', 'chain.a', result=53)
            assert daemon_pid() == pid
            reject('chain.a', 'main.cpp', error="undefined symbol 'head'", env=daemon)
        finally:
            run([ycxx, 'ctl', 'shutdown'], env=base)


if __name__ == '__main__':
    main()
