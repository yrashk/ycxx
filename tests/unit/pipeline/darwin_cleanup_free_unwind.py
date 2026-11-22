"""Check arm64 DWARF rows, native link preservation, and hosted exception propagation."""

import os
from pathlib import Path
import resource
import struct
import subprocess
import sys
import tempfile
from typing import NamedTuple

from scalar_aggregate_unwind import check_scalar_aggregate_unwind, fixtures as scalar_aggregate_fixtures


class Section(NamedTuple):
    index: int
    address: int
    size: int
    offset: int
    alignment: int
    reloc_offset: int
    reloc_count: int
    flags: int


class Image:
    def __init__(self, path, kind):
        self.data = path.read_bytes()
        magic, cpu, _, file_kind, commands = struct.unpack_from('<5I', self.data)
        assert (magic, cpu, file_kind) == (0xfeedfacf, 0x0100000c, kind)
        self.sections = {}
        self.symbols = []
        cursor = 32
        for _ in range(commands):
            command, length = struct.unpack_from('<II', self.data, cursor)
            if command == 0x19:  # LC_SEGMENT_64
                count, = struct.unpack_from('<I', self.data, cursor + 64)
                for i in range(count):
                    values = struct.unpack_from('<16s16sQQIIIII', self.data, cursor + 72 + 80 * i)
                    name, segment, *fields = values
                    key = (segment.rstrip(b'\0'), name.rstrip(b'\0'))
                    self.sections[key] = Section(len(self.sections) + 1, *fields)
            elif command == 0x2:  # LC_SYMTAB
                table, count, strings, _ = struct.unpack_from('<4I', self.data, cursor + 8)
                for i in range(count):
                    name, type_, section, _, address = struct.unpack_from('<IBBHQ', self.data, table + 16 * i)
                    start = strings + name
                    spelling = self.data[start:self.data.index(0, start)].decode()
                    self.symbols.append((spelling, type_, section, address))
            cursor += length

    def contents(self, section):
        return self.data[section.offset:section.offset + section.size]

    def frames(self):
        return self.sections.get((b'__TEXT', b'__eh_frame'))

    def text(self):
        return self.sections[b'__TEXT', b'__text']


def records(data, terminator):
    cursor = 0
    cies = set()
    fdes = []
    while cursor < len(data):
        length, = struct.unpack_from('<I', data, cursor)
        if length == 0:
            assert terminator and cursor + 4 == len(data)
            return fdes
        cie, = struct.unpack_from('<I', data, cursor + 4)
        end = cursor + 4 + length
        assert cursor + 8 <= end <= len(data) and end % 8 == 0
        if cie == 0:
            cies.add(cursor)
            assert data[cursor + 8:end] == b'\x01zR\x00\x01\x78\x1e\x01\x10\x0c\x1f\x00\x08\x1d\x08\x1e'
        else:
            assert cursor + 4 - cie in cies
            displacement, extent, augmentation = struct.unpack_from('<qQB', data, cursor + 8)
            assert extent > 0 and augmentation == 0
            fdes.append((cursor + 8, displacement, extent, data[cursor + 25:end]))
        cursor = end
    assert not terminator and cies == {0}
    return fdes


def check_rows(cfi, code):
    # Exact Arm instructions from the ycxx-produced image. Each row takes effect after its
    # stack/frame change, including the first instruction of a later block after a return.
    words = [word for word, in struct.iter_unpack('<I', code)]
    assert words[:2] == [0xa9bf7bfd, 0x910003fd]  # stp x29, x30, [sp, #-16]!; add x29, sp, #0
    saved = bytes((0x0c, 31, 16, 0x9d, 2, 0x9e, 1))
    body = bytes((0x0c, 29, 16, 0x9d, 2, 0x9e, 1))
    entry = bytes((0x0c, 31, 0, 0xdd, 0xde))
    expected = [(4, saved), (8, body)]
    returns = 0
    for i, word in enumerate(words):
        if word == 0xd65f03c0:  # ret
            returns += 1
            assert words[i - 2:i] == [0x910003bf, 0xa8c17bfd]  # add sp, x29, #0; ldp x29, x30, [sp], #16
            expected.append((i * 4, entry))
            if i + 1 < len(words):
                expected.append(((i + 1) * 4, body))
    pc = 0
    cursor = 0
    for next_pc, operations in expected:
        advance = cfi[cursor]
        cursor += 1
        if advance in (2, 3, 4):
            size = 1 << (advance - 2)
            delta = int.from_bytes(cfi[cursor:cursor + size], 'little')
            cursor += size
        else:
            assert advance >> 6 == 1
            delta = advance & 0x3f
        pc += delta
        assert pc == next_pc and cfi[cursor:cursor + len(operations)] == operations
        cursor += len(operations)
    assert not any(cfi[cursor:])
    return returns


def object_frames(image, expected):
    text = image.text()
    bytes_ = image.contents(text)
    frame = image.frames()
    functions = {address: name for name, _, section, address in image.symbols if section == text.index}
    assert expected <= set(functions.values()), (expected, functions)
    if not expected:
        assert frame is None
        return {}
    assert frame is not None and frame.alignment == 3 and frame.flags & 0xff == 0x0b
    fdes = records(image.contents(frame), terminator=False)
    assert frame.reloc_count == 2 * len(fdes)
    relocations = {}
    for i in range(len(fdes)):
        r1, minus, r2, plus = struct.unpack_from('<IIII', image.data, frame.reloc_offset + 16 * i)
        # Both relocations address the same eight-byte field and use symbol-table indices.
        assert r1 == r2 and minus >> 24 == 0x1e and plus >> 24 == 0x0e and r1 not in relocations
        _, type_, section, address = image.symbols[minus & 0xffffff]
        assert type_ & 1 == 0 and section == frame.index and address == frame.address
        name, _, section, address = image.symbols[plus & 0xffffff]
        assert section == text.index and name == functions[address]
        relocations[r1] = address
    covered = {}
    boundaries = sorted([*functions, text.address + text.size])
    for field, displacement, extent, cfi in fdes:
        start = relocations.pop(field)
        assert displacement == -field and start + extent == boundaries[boundaries.index(start) + 1]
        offset = start - text.address
        name = functions[start]
        assert name not in covered
        covered[name] = extent
        returns = check_rows(cfi, bytes_[offset:offset + extent])
        if name == '_bridge':
            assert returns == 2
    assert not relocations and covered.keys() == expected, (covered, expected)
    return covered


def check_native(path, covered):
    image = Image(path, kind=2)
    frame = image.frames()
    assert frame is not None and frame.reloc_count == 0
    actual = sorted((frame.address + field + displacement, extent)
                    for field, displacement, extent, _ in records(image.contents(frame), terminator=True))
    expected = sorted((address, covered[name]) for name, _, section, address in image.symbols
                      if section == image.text().index and name in covered)
    assert len(expected) == len(covered) and actual == expected, (actual, expected)


def main():
    ycxx, *host = sys.argv[1:]
    fixtures = Path(__file__).with_suffix('').resolve()
    env = dict(os.environ, LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='1')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='ycxx-darwin-unwind-', dir='.') as temporary:
        scope = Path(temporary).resolve()
        (scope / '.ycxx').touch()

        def run(*args, status=0, error=''):
            result = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=True, timeout=60)
            assert result.returncode == status and error in result.stderr, (args, result)

        def compile(*args, **options):
            run(ycxx, 'cxx', '--target=arm64-apple-darwin', '-std=c++26', *args, **options)

        compile('-c', fixtures / 'bridge.cpp', '-o', 'bridge.o')
        frames = object_frames(Image(scope / 'bridge.o', kind=1), {'_bridge', '_scalar_helper', '_callback_helper'})
        compile('-c', fixtures / 'excluded.cpp', '-o', 'excluded.o')
        excluded = Image(scope / 'excluded.o', kind=1)
        names = {name for name, _, section, _ in excluded.symbols if section == excluded.text().index}
        assert {'_excluded_noexcept', '_excluded_indirect', '_excluded_cleanup'} <= names
        assert any('excluded_inline' in name for name in names)
        assert any('excluded_specialization' in name for name in names)
        object_frames(excluded, {'_use_inline'})

        redeclarations = fixtures.with_name('linux_cleanup_free_unwind') / 'omission/written-redeclarations.cpp'
        compile('-c', redeclarations, '-o', 'redeclarations.o')
        object_frames(Image(scope / 'redeclarations.o', kind=1), {'_eligible'})

        compile('-c', fixtures / 'native_throw.cpp', '-o', 'throw.o', status=1, error='`throw` is not in the')
        assert not (scope / 'throw.o').exists()

        def check_scalar_aggregate(obj, functions):
            object_frames(Image(scope / obj, kind=1), {'_' + name for name in functions})

        # PARITY: by design: the records are checked everywhere and only macOS runs the Mach-O peers.
        check_scalar_aggregate_unwind(compile, run, check_scalar_aggregate, host[0] if host else None)
        compile('-c', scalar_aggregate_fixtures / 'unsupported_cleanup.cpp', '-o', 'unsupported_cleanup.o')
        unsupported = Image(scope / 'unsupported_cleanup.o', kind=1)
        assert any(name == '__Z15guarded_defaultv' and section == unsupported.text().index
                   for name, _, section, _ in unsupported.symbols)
        object_frames(unsupported, set())

        compile('-c', fixtures / 'native_peer.cpp', '-o', 'native.o')
        frames.update(object_frames(Image(scope / 'native.o', kind=1), {'_peer'}))
        compile('bridge.o', 'native.o', '-o', 'native')
        check_native(scope / 'native', frames)
        # PARITY: by design: only an arm64 macOS host can execute these linked Mach-O images.
        if host:
            run('./native')
            run(host[0], '-std=c++20', '-pedantic-errors', '-O0', '-fexceptions', '-fno-lto',
                fixtures / 'peer.cpp', 'bridge.o', '-o', 'hosted')
            run('./hosted')


if __name__ == '__main__':
    main()
