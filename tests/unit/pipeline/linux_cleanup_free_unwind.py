"""Check ycxx frame data, hosted exception propagation, and native ELF preservation."""

import os
from pathlib import Path
import re
import resource
import shutil
import struct
import subprocess
import sys
import tempfile
from typing import NamedTuple


class Section(NamedTuple):
    name: int
    type: int
    flags: int
    address: int
    offset: int
    size: int
    link: int
    info: int
    alignment: int
    entry_size: int


def string_at(table, start):
    return table[start:table.index(0, start)].decode()


def frame_records(data):
    cies = set()
    fdes = []
    cursor = 0
    while cursor < len(data):
        length, cie = struct.unpack_from('<II', data, cursor)
        end = cursor + 4 + length
        assert cursor + 8 <= end <= len(data) and end % 8 == 0
        if cie == 0:
            cies.add(cursor)
            # Version, zR, alignment factors, return column, and pcrel sdata4 encoding.
            assert data[cursor + 8:cursor + 17] == b'\x01zR\x00\x01\x78\x10\x01\x1b'
        else:
            assert cursor + 4 - cie in cies
            displacement, extent, augmentation_size = struct.unpack_from('<iiB', data, cursor + 8)
            assert extent > 0 and augmentation_size == 0
            fdes.append((cursor + 8, displacement, extent))
        cursor = end
    return fdes


def object_frames(path):
    data = path.read_bytes()
    assert data[:7] == b'\x7fELF\x02\x01\x01' and struct.unpack_from('<HH', data, 16) == (1, 62)
    section_offset, = struct.unpack_from('<Q', data, 40)
    section_size, count, names_index = struct.unpack_from('<HHH', data, 58)
    sections = [Section(*struct.unpack_from('<IIQQQQIIQQ', data, section_offset + i * section_size))
                for i in range(count)]

    def contents(section):
        return data[section.offset:section.offset + section.size]

    names = contents(sections[names_index])
    indices = {string_at(names, section.name): i for i, section in enumerate(sections)}
    frame_index = indices['.eh_frame']
    frame = sections[frame_index]
    assert (frame.type, frame.flags, frame.alignment) == (0x70000001, 2, 8)
    code_index = indices['.text']
    code = sections[code_index]
    rela = sections[indices['.rela.eh_frame']]
    assert (rela.type, rela.info, rela.entry_size) == (4, frame_index, 24)
    symtab = sections[rela.link]
    symbols = list(struct.iter_unpack('<IBBHQQ', contents(symtab)))
    strings = contents(sections[symtab.link])
    functions = {string_at(strings, name): (info >> 4, offset, size)
                 for name, info, _, section, offset, size in symbols
                 if info & 15 == 2 and section == code_index}
    fdes = frame_records(contents(frame))
    relocations = list(struct.iter_unpack('<QQq', contents(rela)))
    assert len(fdes) == len(relocations)
    covered = []
    for (field, zero, extent), (offset, info, addend) in zip(fdes, relocations):
        assert (field, zero, info & 0xffffffff, addend) == (offset, 0, 2, 0)
        _, binding, _, section, start, _ = symbols[info >> 32]
        assert binding >> 4 == 0 and section == code_index
        covered.append((start, extent))
    assert len(set(covered)) == len(covered)
    return code, functions, covered, section_offset + frame_index * section_size


def check_physical_rows(frames, instructions, functions):
    assert 'CIE "zR" cf=1 df=-8 ra=16' in frames
    reported = {}
    for line in frames.splitlines():
        if match := re.search(r'\bFDE\b.*pc=([0-9a-f]+)\.\.([0-9a-f]+)', line):
            region = tuple(int(value, 16) for value in match.groups())
            reported[region] = []
        elif reported and (match := re.fullmatch(r'([0-9a-f]+)\s+(\S+)\s+(\S+)\s+(\S+)\s*', line)):
            addr, cfa, rbp, rip = match.groups()
            reported[region].append((int(addr, 16), cfa, rbp, rip))

    decoded = [(int(addr, 16), operation) for addr, operation in re.findall(
        r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2}\s+)+([a-z][a-z0-9]*)', instructions, re.MULTILINE)]
    for _, start, extent in functions.values():
        end = start + extent
        body = [(addr, op) for addr, op in decoded if start <= addr < end]
        assert [op.removesuffix('q') for _, op in body[:2]] == ['push', 'mov']
        expected = [(start, 'rsp+8', 's', 'c-8'), (body[1][0], 'rsp+16', 'c-16', 'c-8'),
                    (body[2][0], 'rbp+16', 'c-16', 'c-8')]
        restored_block = False
        for index, (addr, operation) in enumerate(body):
            if operation.removesuffix('q') != 'ret':
                continue
            assert body[index - 1][1].removesuffix('q') == 'leave'
            expected.append((addr, 'rsp+8', 's', 'c-8'))
            if index + 1 < len(body):
                expected.append((body[index + 1][0], 'rbp+16', 'c-16', 'c-8'))
                restored_block = True
        assert restored_block and expected == reported[(start, end)], (expected, reported)


def check_native_frames(path, inputs):
    data = path.read_bytes()
    assert struct.unpack_from('<H', data, 16) == (2,)
    offset, = struct.unpack_from('<Q', data, 32)
    size, count = struct.unpack_from('<HH', data, 54)
    entries = [struct.unpack_from('<IIQQQQQQ', data, offset + i * size) for i in range(count)]
    (_, _, file_offset, mapped, _, file_size, _, _), = (p for p in entries if p[:2] == (1, 4))
    (_, _, _, executable, _, _, _, _), = (p for p in entries if p[:2] == (1, 5))
    preserved = [(mapped + field + displacement, extent) for field, displacement, extent
                 in frame_records(data[file_offset:file_offset + file_size])]
    expected = []
    code_at = executable + offset + size * count
    for code, _, framed, _ in inputs:
        code_at = (code_at + code.alignment - 1) & -code.alignment
        expected += [(code_at + start, extent) for start, extent in framed]
        code_at += code.size
    assert preserved == expected, (preserved, expected)


def main():
    ycxx, peer, readelf, objdump = sys.argv[1:]
    fixtures = Path(__file__).with_suffix('').resolve()
    target = '--target=x86_64-pc-linux-gnu'
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='ycxx-unwind-', dir='.') as temporary, \
            tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temporary).resolve()
        (scope / '.ycxx').touch()
        env = {key: value for key, value in os.environ.items()
               if key not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT')}
        env.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='1')
        cold = dict(env, YCXX_NO_DAEMON='1')
        daemon = dict(env, YCXX_DAEMON='require')

        def run(*args, status=0, context=cold):
            result = subprocess.run(args, cwd=scope, env=context, timeout=30, capture_output=True, text=True)
            assert result.returncode == status, (args, result.returncode, status, result.stderr)
            return result.stdout

        def compile(*args, context=cold):
            return run(ycxx, 'cxx', target, '-std=c++26', *args, context=context)

        compile('-c', fixtures / 'hosted/frames.cpp', '-o', 'frames.o')
        _, functions, covered, _ = object_frames(scope / 'frames.o')
        assert functions.keys() == {'amber_inner', 'amber_outer'}
        assert all(binding == 1 for binding, _, _ in functions.values())
        assert set(covered) == {(start, size) for _, start, size in functions.values()}
        assert functions['amber_outer'][1] > 0
        check_physical_rows(run(readelf, '-W', '--debug-dump=frames-interp', 'frames.o'),
                            run(objdump, '-dw', 'frames.o'), functions)
        run(peer, '-std=c++26', '-pedantic-errors', '-nostdinc', '-nostdinc++', '-O0', '-fexceptions',
            '-fno-inline', '-fno-optimize-sibling-calls', '-fno-lto', '-c', fixtures / 'hosted/peer.cpp', '-o', 'peer.o')
        run(peer, '-fno-lto', 'peer.o', 'frames.o', '-o', 'hosted')
        run('./hosted', 'normal')
        run('./hosted')

        omissions = {
            'guard': ('guarded_path',),
            'inline': ('inline_path',),
            'local-cleanup': ('cleanup_path',),
            'noexcept': ('terminating_path',),
            'synthetic-lambda': ('__invoke', '_clEi', 'request_lambda_path'),
            'template': ('generated_pathI', 'request_generated_path'),
            'written-redeclarations': ('prior', 'later', 'explicitly_nonthrowing'),
        }
        for fixture, markers in omissions.items():
            compile('-c', fixtures / 'omission' / f'{fixture}.cpp', '-o', 'omission.o')
            _, functions, covered, _ = object_frames(scope / 'omission.o')
            companion = 'eligible' if fixture == 'written-redeclarations' else 'ordinary_path'
            assert functions[companion][1:] in covered
            for marker in markers:
                excluded = [functions[marker]] if marker in functions else [
                    symbol for name, symbol in functions.items() if name.startswith('_Z') and marker in name]
                assert len(excluded) == 1 and excluded[0][1:] not in covered, (fixture, marker, functions, covered)
                if fixture == 'inline':
                    assert excluded[0][0] == 2
                if fixture == 'synthetic-lambda' and marker != 'request_lambda_path':
                    assert excluded[0][0] == 0

        one_shot = fixtures / 'freestanding/one-shot.cpp'
        compile('-c', one_shot, '-o', 'single.o')
        compile('-nostdlib', one_shot, '-o', 'single', context=dict(cold, PATH=''))
        run('./single', status=42)
        check_native_frames(scope / 'single', [object_frames(scope / 'single.o')])

        names = ('stage-main', 'stage-provider')
        sources = [f'{name}.cpp' for name in names]
        objects = [f'{name}.o' for name in names]
        for source, obj in zip(sources, objects):
            shutil.copyfile(fixtures / 'freestanding' / source, scope / source)
            compile('-c', source, '-o', obj)
        object_metadata = [object_frames(scope / obj) for obj in objects]
        compile('-nostdlib', *objects, '-o', 'staged', context=dict(cold, PATH=''))
        compile('-nostdlib', *sources, '-o', 'one-shot', context=dict(cold, PATH=''))
        run('./staged', status=42)
        run('./one-shot', status=42)
        staged = (scope / 'staged').read_bytes()
        assert staged == (scope / 'one-shot').read_bytes()
        check_native_frames(scope / 'staged', object_metadata)

        # The reader also accepts the same allocated section in the SHT_PROGBITS spelling.
        provider = bytearray((scope / objects[1]).read_bytes())
        header = object_metadata[1][3]
        struct.pack_into('<I', provider, header + 4, 1)
        (scope / 'progbits.o').write_bytes(provider)
        compile('-nostdlib', objects[0], 'progbits.o', '-o', 'progbits', context=dict(cold, PATH=''))
        assert staged == (scope / 'progbits').read_bytes()

        try:
            compile('-nostdlib', *sources, '-o', 'daemon', context=daemon)
            run(ycxx, 'ctl', 'status', context=env)
            run('./daemon', status=42)
            assert staged == (scope / 'daemon').read_bytes()
        finally:
            subprocess.run([ycxx, 'ctl', 'shutdown'], cwd=scope, env=env, timeout=30, capture_output=True)


if __name__ == '__main__':
    main()
