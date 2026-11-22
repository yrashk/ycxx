"""Link arm64 Mach-O executables against Darwin archives through the CLI and the daemon (#842).

The Darwin counterpart of linux_archive.py. Archives come from the system `ar` (BSD names and a
`__.SYMDEF SORTED` ranlib index) or are built here for malformed and 64-bit index fixtures. The
selection rules are the system linker's, checked against it where they differ from ELF's: an
archive serves names referenced anywhere on the command line, weak references and tentative
definitions select members, and the first archive in operand order that lists a name supplies it.
"""

import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def header(name, size):
    text = f'{name:16}{0:<12}{0:<6}{0:<6}{644:<8}{size:<10}`\n'
    assert len(text) == 60
    return text.encode('ascii')


def bsd_archive(members, index=None, *, wide=False, index_name='__.SYMDEF SORTED', index_first=True):
    """A BSD archive whose members are (name, bytes), every name stored inline after `#1/`.

    `index` lists (symbol, member number) entries; the ranlib table is written with each
    member's header offset unless an entry gives an explicit offset as a third element."""
    def member(name, data):
        name = name.encode('ascii')
        padded = name + b'\0' * (-len(name) % 8)
        body = padded + data
        return header(f'#1/{len(padded)}', len(body)) + body + (b'\n' if len(body) % 2 else b'')

    def table(offsets):
        strings, entries = b'', []
        for entry in index:
            strx = len(strings)
            strings += entry[0].encode('ascii') + b'\0'
            entries.append((strx, entry[2] if len(entry) > 2 else offsets[entry[1]]))
        strings += b'\0' * (-len(strings) % 8)
        word = '<Q' if wide else '<I'
        out = struct.pack(word, len(entries) * struct.calcsize(word) * 2)
        for strx, offset in entries:
            out += struct.pack(word, strx) + struct.pack(word, offset)
        return out + struct.pack(word, len(strings)) + strings

    if index is None:
        return b'!<arch>\n' + b''.join(member(n, d) for n, d in members)
    name = index_name.replace('SYMDEF', 'SYMDEF_64') if wide else index_name
    # The index's size does not depend on the offsets, so lay out with a placeholder first.
    size = len(member(name, table([0] * len(members))))
    offsets, at = [], len(b'!<arch>\n') + (size if index_first else 0)
    for n, d in members:
        offsets.append(at)
        at += len(member(n, d))
    parts = [member(n, d) for n, d in members]
    index_member = member(name, table(offsets))
    assert len(index_member) == size
    return b'!<arch>\n' + (index_member + b''.join(parts) if index_first else b''.join(parts) + index_member)


def main():
    ycxx, ar, cc = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='ycxx-darwin-archive-', dir='.') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temp).resolve()
        (scope / '.ycxx').touch()
        base = {k: v for k, v in os.environ.items() if k not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT')}
        base.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')
        cold = dict(base, YCXX_NO_DAEMON='1')
        daemon = dict(base, YCXX_DAEMON='require')

        def run(args, status=0, error='', env=cold):
            result = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=True, timeout=60)
            if result.returncode != status or error not in result.stderr:
                raise AssertionError(f'{args} exited {result.returncode}, expected {status}'
                                     f' and stderr containing {error!r}\n{result.stdout}{result.stderr}')
            return result.stdout

        def source(name, text, out=None):
            (scope / name).write_text(text + '\n')
            if out is not None:
                run([ycxx, 'cxx', '-c', name, '-o', out])

        def archive(out, *members):
            (scope / out).unlink(missing_ok=True)
            run([ar, 'crs', out, *members])

        def link(*inputs, result, env=cold):
            run([ycxx, 'cxx', *inputs, '-o', 'program'], env=env)
            run(['./program'], status=result)
            return (scope / 'program').read_bytes()

        def system_link(*inputs, result):
            """The same link by the system linker, the oracle for selection."""
            run([cc, *inputs, '-o', 'host-program'])
            run(['./host-program'], status=result)

        def reject(*args, error, env=cold):
            run([ycxx, 'cxx', *args, '-o', 'rejected'], status=1, error=error, env=env)
            assert not (scope / 'rejected').exists()

        def daemon_pid():
            status = run([ycxx, 'ctl', 'status'], env=base)
            pid, = (line.removeprefix('pid: ') for line in status.splitlines() if line.startswith('pid: '))
            return int(pid)

        source('main.cpp', 'extern "C" int head(); int main() { return head(); }', 'main.o')
        source('head.cpp', 'extern "C" int tail(); extern "C" int head() { return tail(); }',
               'selected member with a long name.o')
        source('tail.cpp', 'extern "C" int tail() { return 37; }', 'tail.o')
        (scope / 'unselected data.bin').write_bytes(b'odd')
        archive('chain.a', 'tail.o', 'selected member with a long name.o', 'unselected data.bin')
        listed = run([ar, 't', 'chain.a']).splitlines()
        assert listed[0].startswith('__.SYMDEF') and listed[1:3] == ['tail.o', 'selected member with a long name.o'], listed
        assert b'#1/' in (scope / 'chain.a').read_bytes()  # BSD inline names
        image = link('main.cpp', 'chain.a', result=37)
        assert link('main.o', 'chain.a', result=37) == image
        # Unlike an ELF link, an archive serves references from later operands too. The system
        # linker gets an archive of objects only: macOS 15's rejects a member that is not Mach-O.
        archive('objects.a', 'tail.o', 'selected member with a long name.o')
        for order in (('main.o', 'objects.a'), ('objects.a', 'main.o')):
            system_link(*order, result=37)
            assert link(*order, result=37) == image
        assert link('chain.a', 'main.o', result=37) == image
        assert link('chain.a', 'main.cpp', 'chain.a', result=37) == image

        archive('head.a', 'selected member with a long name.o')
        archive('tail.a', 'tail.o')
        system_link('main.o', 'tail.a', 'head.a', result=37)
        assert link('main.o', 'tail.a', 'head.a', result=37) == image
        assert link('main.o', 'head.a', 'tail.a', result=37) == image

        # The first archive in operand order that lists a name supplies it.
        source('other-tail.cpp', 'extern "C" int tail() { return 41; }', 'other-tail.o')
        archive('other.a', 'other-tail.o')
        system_link('main.o', 'head.a', 'other.a', 'tail.a', result=41)
        link('main.o', 'head.a', 'other.a', 'tail.a', result=41)
        system_link('main.o', 'tail.a', 'other.a', 'head.a', result=37)
        link('main.o', 'tail.a', 'other.a', 'head.a', result=37)
        # An object's definition wins over every archive, wherever the object comes.
        system_link('main.o', 'objects.a', 'other-tail.o', result=41)
        link('main.o', 'chain.a', 'other-tail.o', result=41)

        # A weak reference selects a member, as the system linker does.
        (scope / 'weak.c').write_text('extern int tail(void) __attribute__((weak));\n'
                                      'int main(void) { return tail ? tail() : 3; }\n')
        run([cc, '-c', 'weak.c'])
        assert 'weak external _tail' in run(['nm', '-m', 'weak.o'])
        system_link('weak.o', 'tail.a', result=37)
        link('weak.o', 'tail.a', result=37)
        link('weak.o', result=3)

        # A tentative definition selects a member that defines the name, as macOS 26's linker
        # does. macOS 15's keeps the tentative definition (exit 0), so no oracle here.
        (scope / 'common.c').write_text('int shared;\nint main(void) { return shared; }\n')
        (scope / 'shared.c').write_text('int shared = 5;\n')
        run([cc, '-fcommon', '-c', 'common.c', 'shared.c'])
        archive('shared.a', 'shared.o')
        link('common.o', 'shared.a', result=5)

        # An archive member wins over libSystem.
        source('puts-main.cpp', 'extern "C" int puts(const char*); int main() { return puts("x"); }', 'puts-main.o')
        source('puts.cpp', 'extern "C" int puts(const char*) { return 11; }', 'puts.o')
        archive('puts.a', 'puts.o')
        system_link('puts-main.o', 'puts.a', result=11)
        link('puts-main.o', 'puts.a', result=11)

        # A global in a section the linker drops (`__LLVM`) defines nothing, so it neither stops
        # selection nor counts as the selected member's definition.
        (scope / 'marker.c').write_text('__attribute__((section("__LLVM,__marker"))) char tail = 1;\n')
        run([cc, '-c', 'marker.c'])
        assert '(__LLVM,__marker) external _tail' in run(['nm', '-m', 'marker.o'])
        source('tail-main.cpp', 'extern "C" int tail(); int main() { return tail(); }', 'tail-main.o')
        link('tail-main.o', 'marker.o', 'tail.a', result=37)
        reject('tail-main.o', 'marker.o', error='undefined symbol _tail')
        (scope / 'marker-index.a').write_bytes(
            bsd_archive([('marker.o', (scope / 'marker.o').read_bytes())], [('_tail', 0)]))
        reject('tail-main.o', 'marker-index.a',
               error="marker-index.a(marker.o): archive index symbol '_tail' is not defined by the member")

        # A member is read only when selected.
        (scope / 'garbage.o').write_bytes(b'not an object')
        tail_bytes = (scope / 'tail.o').read_bytes()
        (scope / 'lazy.a').write_bytes(bsd_archive([('tail.o', tail_bytes), ('garbage.o', b'not an object')],
                                                   [('_tail', 0), ('_nothing', 1)]))
        source('unrelated.cpp', 'int main() { return 9; }')
        link('unrelated.cpp', 'lazy.a', result=9)
        source('wants-garbage.cpp', 'extern "C" int nothing(); int main() { return nothing(); }')
        reject('wants-garbage.cpp', 'lazy.a', error='lazy.a(garbage.o): not a 64-bit Mach-O file')
        (scope / 'liar.a').write_bytes(bsd_archive([('tail.o', tail_bytes)], [('_head', 0)]))
        reject('main.cpp', 'liar.a', error="liar.a(tail.o): archive index symbol '_head' is not defined by the member")

        # Built here: both index names, 32-bit and 64-bit, and an empty archive.
        head_bytes = (scope / 'selected member with a long name.o').read_bytes()
        for wide in (False, True):
            for name in ('__.SYMDEF', '__.SYMDEF SORTED'):
                (scope / 'built.a').write_bytes(bsd_archive(
                    [('tail.o', tail_bytes), ('head.o', head_bytes)], [('_head', 1), ('_tail', 0)],
                    wide=wide, index_name=name))
                assert link('main.o', 'built.a', result=37) == image
        (scope / 'empty.a').write_bytes(b'!<arch>\n')
        assert link('unrelated.cpp', 'empty.a', result=9) == link('unrelated.cpp', result=9)

        reject('chain.a', error='an archive link requires at least one source or ordinary object')
        reject('-c', 'main.cpp', 'chain.a', error='archive inputs require an executable link')
        reject('main.cpp', 'absent.a', error="cannot read 'absent.a'")

        chain = (scope / 'chain.a').read_bytes()
        good = bsd_archive([('tail.o', tail_bytes)], [('_tail', 0)])
        index_at = 8 + 60 + 16  # the ranlib array's size, after the index's inline name '__.SYMDEF SORTED'
        for data, diagnostic in (
            (b'invalid', 'expected a Darwin archive'),
            (b'!<thin>\n', 'thin archives are unsupported'),
            (b'!<arch>\npart', 'truncated archive member header'),
            (chain[:66] + b'!!' + chain[68:], 'invalid archive member header trailer'),
            (chain[:56] + b'4294967296' + chain[66:], 'archive member contents exceed the file'),
            (chain[:56] + b'1x        ' + chain[66:], 'invalid archive member size'),
            (b'!<arch>\n' + header('#1/99', 4) + b'name', 'invalid BSD archive member name length'),
            (b'!<arch>\n' + header('#1/4', 4) + b'\0\0\0\0', 'invalid archive member name'),
            (b'!<arch>\n' + header('/', 4) + bytes(4), 'GNU archive indexes and long names are unsupported'),
            (bsd_archive([('tail.o', tail_bytes)]), 'nonempty archive has no symbol index'),
            (bsd_archive([('tail.o', tail_bytes)], [('_tail', 0)], index_first=False),
             'the archive symbol index is not the first member'),
            (good[:index_at] + struct.pack('<I', 12) + good[index_at + 4:], 'invalid archive symbol index size'),
            (good[:index_at] + struct.pack('<I', 1 << 20) + good[index_at + 4:], 'invalid archive symbol index size'),
            (good[:index_at + 12] + struct.pack('<I', 1 << 20) + good[index_at + 16:],
             'invalid archive symbol string table size'),
            (good[:index_at + 4] + struct.pack('<I', 64) + good[index_at + 8:],
             'archive index symbol name lies outside the string table'),
            (bsd_archive([('tail.o', tail_bytes)], [('_tail', 0, 9)]), 'invalid archive index member offset'),
            (bsd_archive([('tail.o', tail_bytes)], [('', 0)]), 'empty archive index symbol'),
        ):
            (scope / 'broken.a').write_bytes(data)
            reject('main.cpp', 'broken.a', error=diagnostic)
        # A string table without a terminator: its last name runs to its end.
        unterminated = bytearray(good)
        strings_size = struct.unpack_from('<I', unterminated, index_at + 12)[0]
        strings_at = index_at + 16
        unterminated[strings_at:strings_at + strings_size] = b'_' * strings_size
        (scope / 'broken.a').write_bytes(bytes(unterminated))
        reject('main.cpp', 'broken.a', error='unterminated archive index symbol')
        # Container errors are reported even when no member would be selected.
        (scope / 'broken.a').write_bytes(bsd_archive([('tail.o', tail_bytes)], [('_tail', 0, 9)]))
        reject('unrelated.cpp', 'broken.a', error='invalid archive index member offset')

        try:
            assert link('main.cpp', 'chain.a', result=37, env=daemon) == image
            pid = daemon_pid()
            assert link('main.cpp', 'chain.a', result=37, env=daemon) == image
            source('tail.cpp', 'extern "C" int tail() { return 53; }', 'tail.o')
            archive('replacement.a', 'tail.o', 'selected member with a long name.o', 'unselected data.bin')
            (scope / 'replacement.a').replace(scope / 'chain.a')
            replacement = link('main.cpp', 'chain.a', result=53, env=daemon)
            assert replacement != image and replacement == link('main.cpp', 'chain.a', result=53)
            assert daemon_pid() == pid
        finally:
            run([ycxx, 'ctl', 'shutdown'], env=base)


if __name__ == '__main__':
    main()
