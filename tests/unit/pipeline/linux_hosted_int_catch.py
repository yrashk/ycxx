"""Check Linux x86-64 hosted int matching, ELF type metadata, and resident edits."""

from collections import namedtuple
import os
from pathlib import Path
import resource
import struct
import subprocess
import sys
import tempfile


Section = namedtuple('Section', 'name kind flags address offset size link info alignment entry_size')
Symbol = namedtuple('Symbol', 'name info other section value size')
Relocation = namedtuple('Relocation', 'offset kind symbol addend')
PC32 = 2
ABS64 = 1


def uleb(data, offset):
    value = 0
    for shift in range(0, 64, 7):
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7f) << shift
        if byte < 0x80:
            return value, offset
    raise AssertionError('invalid ULEB128 in LSDA')


def check_object(data, *, typed):
    assert data[:7] == b'\x7fELF\x02\x01\x01'
    assert struct.unpack_from('<HH', data, 16) == (1, 62)  # ET_REL, EM_X86_64
    table, = struct.unpack_from('<Q', data, 40)
    width, count, strings = struct.unpack_from('<HHH', data, 58)
    assert width == 64
    sections = [Section(*fields) for fields in struct.iter_unpack('<IIQQQQIIQQ', data[table:table + width * count])]

    def contents(section):
        return data[section.offset:section.offset + section.size]

    def name(table, offset):
        return table[offset:table.index(b'\0', offset)].decode()

    section_names = contents(sections[strings])
    indices = {name(section_names, section.name): i for i, section in enumerate(sections)}
    symtab, = (section for section in sections if section.kind == 2)
    symbols = [Symbol(*fields) for fields in struct.iter_unpack('<IBBHQQ', contents(symtab))]
    strings = contents(sections[symtab.link])
    symbols = [symbol._replace(name=name(strings, symbol.name)) for symbol in symbols]
    relocations = {
        section.info: [Relocation(offset, info & 0xffffffff, symbols[info >> 32], addend)
                       for offset, info, addend in struct.iter_unpack('<QQq', contents(section))]
        for section in sections if section.kind == 4
    }

    def target(relocation):
        return relocation.symbol.section, relocation.symbol.value + relocation.addend

    def references(spelling):
        return [(index, row) for index, rows in relocations.items() for row in rows if row.symbol.name == spelling]

    text_index = indices['.text']
    handler, = (symbol for symbol in symbols if symbol.name == 'typed_handler')
    assert handler.section == text_index and handler.size > 0, handler
    for spelling in ('peer', '__cxa_begin_catch', '__cxa_end_catch'):
        (index, row), = references(spelling)
        assert index == text_index and handler.value <= row.offset < handler.value + handler.size, row

    lsda_index = indices['.gcc_except_table']
    lsda = contents(sections[lsda_index])
    # Omitted LPStart, then the PC-relative four-byte type encoding (indirect for int).
    assert lsda[:2] == bytes((0xff, 0x9b if typed else 0x1b)), lsda.hex()
    type_offset, cursor = uleb(lsda, 2)
    type_base = cursor + type_offset
    type_slot = type_base - 4
    assert type_base == len(lsda) and lsda[cursor] == 1, lsda.hex()  # ULEB128 call sites.
    call_sites_size, cursor = uleb(lsda, cursor + 1)
    actions = cursor + call_sites_size
    protected_sites = []
    while cursor < actions:
        start, cursor = uleb(lsda, cursor)
        length, cursor = uleb(lsda, cursor)
        landing, cursor = uleb(lsda, cursor)
        action, cursor = uleb(lsda, cursor)
        if landing:
            assert action == 1
            protected_sites.append((start, length, landing))
        else:
            assert action == 0
    (start, length, landing), = protected_sites
    (_, peer), = references('peer')
    assert start <= peer.offset - handler.value < start + length <= handler.size
    assert 0 < landing < handler.size
    assert cursor == actions and lsda[actions:actions + 2] == b'\x01\0', lsda.hex()  # Filter 1, no chain.
    padding = lsda[actions + 2:type_slot]
    assert 0 <= type_slot - actions - 2 < 4 and not any(padding), lsda.hex()  # A single type slot.

    frame_index = indices['.eh_frame']
    assert b'zPLR\0' in contents(sections[frame_index])
    frame_targets = {target(row) for row in relocations[frame_index]
                     if row.kind == PC32 and row.symbol.info >> 4 == 0}
    assert (lsda_index, 0) in frame_targets, frame_targets
    (personality_index, personality), = references('__gxx_personality_v0')
    assert personality.kind == ABS64 and (personality_index, personality.offset) in frame_targets, personality

    if not typed:
        assert lsda[type_slot:] == bytes(4) and not relocations.get(lsda_index) and not references('_ZTIi')
        return

    type_pointer, = relocations[lsda_index]
    assert (type_pointer.offset, type_pointer.kind, type_pointer.symbol.info >> 4) == (type_slot, PC32, 0), type_pointer
    cell_index, cell_offset = target(type_pointer)
    cell = sections[cell_index]
    assert cell.kind == 1 and cell.flags & 2 and not cell.flags & 4, cell  # Allocated, non-executable PROGBITS.
    assert cell.alignment >= 8 and cell_offset % 8 == 0 and 0 <= cell_offset <= cell.size - 8, cell
    assert (cell_index, cell_offset) != (personality_index, personality.offset)
    (index, int_type), = references('_ZTIi')
    assert (index, int_type.offset, int_type.kind, int_type.symbol.section, int_type.addend) == (
        cell_index, cell_offset, ABS64, 0, 0), int_type


def main():
    ycxx, peer = sys.argv[1:]
    fixtures = Path(__file__).resolve().with_suffix('')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='ycxx-int-catch-', dir='.') as temporary, \
            tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temporary).resolve()
        (scope / '.ycxx').touch()
        env = {key: value for key, value in os.environ.items()
               if key not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT', 'YCXX_NO_WATCH')}
        env.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='1', XDG_CACHE_HOME=str(scope / 'cache'))
        cold = dict(env, YCXX_NO_DAEMON='1')
        resident = dict(env, YCXX_DAEMON='require')

        def run(*args, context=cold, status=0, error=''):
            result = subprocess.run(args, cwd=scope, env=context, capture_output=True, text=True, timeout=40)
            assert result.returncode == status and error in result.stderr, (args, status, result)
            return result

        flags = (ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26')
        source = scope / 'handler.cpp'
        obj = scope / 'handler.o'

        def build(context, *, supported=True):
            obj.unlink(missing_ok=True)
            run(*flags, '-fsyntax-only', source.name, context=context)
            if not supported:
                result = run(*flags, '-c', source.name, '-o', obj.name, context=context, status=1,
                             error='with a retained try block')
                assert not obj.exists()
                return result.stdout, result.stderr
            run(*flags, '-c', source.name, '-o', obj.name, context=context)
            return obj.read_bytes()

        def daemon_pid():
            result = run(ycxx, 'ctl', 'status', context=env)
            pid, = (line for line in result.stdout.splitlines() if line.startswith('pid: '))
            return pid

        typed = (fixtures / source.name).read_bytes()
        source.write_bytes(typed)
        first = build(cold)
        check_object(first, typed=True)
        run(*flags, '-nostdlib', obj.name, '-o', 'native', status=1,
            error='native ELF linking of exception handlers is unsupported')
        assert not (scope / 'native').exists()
        run(peer, '-std=c++26', '-pedantic-errors', '-nostdinc', '-nostdinc++', '-O0', '-fPIE',
            '-fexceptions', '-fno-lto', '-c', fixtures / 'peer.cpp', '-o', 'peer.o')
        run(peer, '-pie', '-Wl,-z,text,--wrap=__cxa_begin_catch,--wrap=__cxa_end_catch',
            '-fno-lto', obj.name, 'peer.o', '-o', 'hosted')
        run('./hosted')

        catch_all = typed.replace(b'catch (int value) { return value; }', b'catch (...) { return 53; }')
        short = typed.replace(b'catch (int value)', b'catch (short value)')
        same_unit = typed.replace(b'int peer(int);', b'int peer(int x) { return x; }')
        assert all(edit != typed for edit in (catch_all, short, same_unit))
        try:
            assert build(resident) == first
            pid = daemon_pid()
            for edit, shape in ((catch_all, 'catch-all'), (short, 'unsupported'), (typed, 'typed'),
                                (same_unit, 'unsupported'), (typed, 'typed')):
                source.write_bytes(edit)
                supported = shape != 'unsupported'
                current = build(cold, supported=supported)
                assert current == build(resident, supported=supported), shape
                if shape == 'catch-all':
                    check_object(current, typed=False)
                if shape == 'typed':
                    assert current == first
            assert daemon_pid() == pid
        finally:
            subprocess.run([ycxx, 'ctl', 'shutdown'], cwd=scope, env=env, capture_output=True, timeout=40)


if __name__ == '__main__':
    main()
