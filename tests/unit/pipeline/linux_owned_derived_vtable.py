"""Exchange a primary derived virtual table and RTTI with Linux C++ peers."""

from collections import namedtuple
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile

from linux_compile import run
from linux_peer_derived_default_construction import native_compilers


# Itanium C++ ABI sections 2.5.2, 2.9 and 5.2.3 through 5.2.4.
TABLE, KEY = '_ZTV1D', '_ZN1D1fEv'
DERIVED, BASE = '_ZTI1D', '_ZTI1B'
DERIVED_NAME, BASE_NAME = '_ZTS1D', '_ZTS1B'
SI_RUNTIME = '_ZTVN10__cxxabiv120__si_class_type_infoE'
BASE_RUNTIME = '_ZTVN10__cxxabiv117__class_type_infoE'

Section = namedtuple('Section', 'name kind flags address offset size link info alignment entrysize')
Symbol = namedtuple('Symbol', 'name info other section value size')
Relocation = namedtuple('Relocation', 'section offset kind symbol addend')


class Elf:
    def __init__(self, path):
        self.path = path
        self.data = path.read_bytes()
        assert self.data[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<HH', self.data, 16) == (1, 62), path
        table, = struct.unpack_from('<Q', self.data, 40)
        width, count, self.section_names = struct.unpack_from('<HHH', self.data, 58)
        assert width == 64, path
        headers = self.data[table:table + width * count]
        self.sections = [Section(*entry) for entry in struct.iter_unpack('<IIQQQQIIQQ', headers)]
        self.symtab, = (i for i, section in enumerate(self.sections) if section.kind == 2)
        symbols = self.sections[self.symtab]
        assert symbols.entrysize == 24, path
        self.symbols = [Symbol(*entry) for entry in struct.iter_unpack('<IBBHQQ', self.contents(self.symtab))]
        self.names = [self.string(symbols.link, symbol.name) for symbol in self.symbols]
        self.relocations = {}
        for index, section in enumerate(self.sections):
            if section.kind == 4:
                assert section.link == self.symtab and section.entrysize == 24, path
                entries = self.relocations.setdefault(section.info, [])
                for offset, info, addend in struct.iter_unpack('<QQq', self.contents(index)):
                    entries.append(Relocation(index, offset, info & 0xffffffff, self.names[info >> 32], addend))

    def contents(self, index):
        section = self.sections[index]
        return self.data[section.offset:section.offset + section.size]

    def string(self, index, offset):
        table = self.contents(index)
        return table[offset:table.index(b'\0', offset)].decode()

    def symbol(self, name):
        found = [entry for spelling, entry in zip(self.names, self.symbols) if spelling == name]
        assert len(found) == 1, (self.path, name, found)
        return found[0]

    def object(self, name, size, bindings):
        symbol = self.symbol(name)
        assert symbol.info in bindings and symbol.other == 0 and symbol.section != 0 and symbol.size == size, (
            self.path, name, symbol)
        return symbol

    def group(self, name, members):
        index, = (i for i, section in enumerate(self.sections)
                  if section.kind == 17 and section.link == self.symtab and self.names[section.info] == name)
        section = self.sections[index]
        assert section.flags == 0 and section.entrysize == 4 and section.size == 4 * (1 + len(members)), self.path
        flags, *actual = struct.unpack('<' + 'I' * (1 + len(members)), self.contents(index))
        assert flags == 1 and sorted(actual) == sorted(members), (self.path, name, actual, members)
        assert all(member > index and self.sections[member].flags & 0x200 for member in members), self.path

    def check_words(self, symbol, expected):
        rows = [row for row in self.relocations.get(symbol.section, ())
                if symbol.value <= row.offset < symbol.value + symbol.size]
        actual = {row.offset - symbol.value: (row.kind, row.symbol, row.addend) for row in rows}
        assert len(rows) == len(expected) and actual == expected, (self.path, actual, expected)
        return {row.section for row in rows}


def check_name(elf, name, payload, bindings):
    symbol = elf.object(name, len(payload), bindings)
    assert elf.contents(symbol.section)[symbol.value:symbol.value + symbol.size] == payload, elf.path
    elf.check_words(symbol, {})
    if symbol.info == 0x21:
        elf.group(name, (symbol.section,))


def check_base(elf):
    base = elf.object(BASE, 16, (0x21,))
    assert elf.string(elf.section_names, elf.sections[base.section].name).startswith('.data.rel.ro'), elf.path
    rela, = elf.check_words(base, {0: (1, BASE_RUNTIME, 16), 8: (1, BASE_NAME, 0)})  # R_X86_64_64
    elf.group(BASE, (base.section, rela))
    assert elf.sections[base.section].size == 16, elf.path
    related = [(target, row.symbol) for target, rows in elf.relocations.items() for row in rows
               if row.symbol in (BASE_RUNTIME, BASE_NAME)]
    assert sorted(related) == sorted(((base.section, BASE_RUNTIME), (base.section, BASE_NAME))), elf.path
    check_name(elf, BASE_NAME, b'1B\0', (0x21,))


def check_owner(elf, *, native=False):
    assert '_ZTV1B' not in elf.names, elf.path
    key = elf.symbol(KEY)
    assert key.info == 0x12 and key.other == 0 and key.section != 0 and key.size > 0, elf.path
    table = elf.object(TABLE, 24, (0x11, 0x21) if native else (0x11,))
    assert elf.contents(table.section)[table.value:table.value + 8] == b'\0' * 8, elf.path
    elf.check_words(table, {8: (1, DERIVED, 0), 16: (1, KEY, 0)})
    derived = elf.object(DERIVED, 24, (0x11, 0x21) if native else (0x11,))
    elf.check_words(derived, {0: (1, SI_RUNTIME, 16), 8: (1, DERIVED_NAME, 0), 16: (1, BASE, 0)})
    check_name(elf, DERIVED_NAME, b'1D\0', (0x11, 0x21) if native else (0x21,))
    check_base(elf)


def check_maker(elf):
    table = elf.symbol(TABLE)
    assert table.info >> 4 == 1 and table.other == 0 and table.section == 0, elf.path
    assert any(row.symbol == TABLE for rows in elf.relocations.values() for row in rows), elf.path
    assert not set(elf.names) & {KEY, DERIVED, BASE, DERIVED_NAME, BASE_NAME, '_ZTV1B'}, elf.path


def main():
    ycxx, host, linker = sys.argv[1:]
    fixtures = Path(__file__).resolve().with_suffix('')
    env = {key: value for key, value in os.environ.items() if key not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='2')
    with tempfile.TemporaryDirectory(prefix='ycxx-owned-derived-table-') as temp:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        for unit in ('base.hpp', 'owner.cpp', 'key.cpp', 'maker.cpp'):
            shutil.copyfile(fixtures / unit, scope / unit)
        for unit in ('owner', 'key', 'maker'):
            run([ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-c', f'{unit}.cpp', '-o', f'{unit}.o'], scope, env)
        for unit in ('owner', 'key'):
            check_owner(Elf(scope / f'{unit}.o'))
        check_maker(Elf(scope / 'maker.o'))

        for compiler in native_compilers(host, scope, env):
            flags = [compiler, '-std=c++20', '-O0', '-pedantic-errors', '-fPIE', '-fno-lto', '-fno-exceptions',
                     '-fno-asynchronous-unwind-tables', '-fno-unwind-tables', '-fdata-sections', '-ffunction-sections']
            if 'clang' in run([compiler, '--version'], scope, env).lower():
                flags.append('-fno-addrsig')
            for unit in ('dispatch', 'peer-opaque', 'peer-construct', 'peer-rtti'):
                run([*flags, '-c', fixtures / f'{unit}.cpp', '-o', f'{unit}.o'], scope, env)
            run([*flags, '-c', fixtures / 'key.cpp', '-o', 'native-key.o'], scope, env)
            check_owner(Elf(scope / 'native-key.o'), native=True)
            check_base(Elf(scope / 'peer-rtti.o'))
            for unit in ('dispatch', 'peer-opaque'):
                opaque = Elf(scope / f'{unit}.o')
                assert not set(opaque.names) & {KEY, TABLE, DERIVED, BASE, DERIVED_NAME, BASE_NAME, '_ZTV1B'}, unit

            def link_and_run(name, *inputs):
                run([compiler, '-no-pie', '-fno-lto', *inputs, '-o', name], scope, env)
                run([f'./{name}'], scope, env)

            link_and_run('opaque', 'owner.o', 'dispatch.o', 'peer-opaque.o')
            link_and_run('split', 'maker.o', 'key.o', 'dispatch.o', 'peer-opaque.o')
            link_and_run('native', 'native-key.o', 'peer-rtti.o', 'peer-construct.o', 'dispatch.o')
            for first, second in (('key.o', 'peer-rtti.o'), ('peer-rtti.o', 'key.o')):
                run([linker, '-r', first, second, '-o', 'combined.o'], scope, env)
                check_base(Elf(scope / 'combined.o'))
                link_and_run('rtti', first, second, 'peer-construct.o', 'dispatch.o')


if __name__ == '__main__':
    main()
