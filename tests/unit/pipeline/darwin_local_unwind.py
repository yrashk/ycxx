"""Check one automatic destructor on both exits from a hosted Darwin call."""

import os
from pathlib import Path
import resource
import struct
import subprocess
import sys
import tempfile

from darwin_cleanup_free_unwind import Image, check_rows


def relocations(image, section):
    start = section.reloc_offset
    end = start + section.reloc_count * 8
    return [(address, word >> 24, image.symbols[word & 0xffffff])
            for address, word in struct.iter_unpack('<II', image.data[start:end])]


def uleb(data, cursor):
    result = 0
    shift = 0
    while True:
        byte = data[cursor]
        cursor += 1
        result |= (byte & 0x7f) << shift
        if byte < 0x80:
            return result, cursor
        shift += 7


def check_object(path):
    image = Image(path, kind=1)
    frame = image.frames()
    assert frame is not None, 'the cleanup frame is missing'
    lsda = image.sections[b'__TEXT', b'__gcc_except_tab']
    text = image.text()
    data = image.contents(frame)
    length, cie_id = struct.unpack_from('<II', data)
    assert cie_id == 0 and data[8:19] == b'\x01zPLR\x00\x01\x78\x1e\x07\x9b'
    personality = 19
    assert struct.unpack_from('<i', data, personality) == (-personality,)
    assert data[23:25] == b'\x10\x10'
    end_cie = 4 + length
    fde_length, cie, displacement, extent, aug_size, lsda_delta = struct.unpack_from('<IIqQBq', data, end_cie)
    pc_field = end_cie + 8
    lsda_field = end_cie + 25
    assert (cie, displacement, aug_size, lsda_delta) == (end_cie + 4, -pc_field, 8, -lsda_field)
    assert end_cie + 4 + fde_length == len(data)

    got, pc_minus, pc_plus, lsda_minus, lsda_plus = relocations(image, frame)
    assert got[:2] == (personality, 0x7d)  # PC-relative POINTER_TO_GOT, four bytes
    assert got[2][0] == '___gxx_personality_v0' and got[2][2] == 0
    for field, minus, plus, target in ((pc_field, pc_minus, pc_plus, text),
                                       (lsda_field, lsda_minus, lsda_plus, lsda)):
        assert minus[:2] == (field, 0x1e) and plus[:2] == (field, 0x0e)
        assert minus[2][1] & 1 == 0 and minus[2][2:] == (frame.index, frame.address)
        assert plus[2][2] == target.index
    assert pc_plus[2][0] == '_checked' and lsda_plus[2][3] == lsda.address

    start = pc_plus[2][3]
    end = min([text.address + text.size, *(address for _, _, section, address in image.symbols
                                           if section == text.index and address > start)])
    assert start + extent == end
    offset = start - text.address
    code = image.contents(text)[offset:offset + extent]
    assert check_rows(data[end_cie + 33:], code) == 1

    table = image.contents(lsda)
    assert table[:3] == b'\xff\xff\x01'  # default LPStart, no type table, ULEB fields
    size, cursor = uleb(table, 3)
    assert size == len(table) - cursor
    sites = []
    while cursor < len(table):
        fields = []
        for _ in range(4):
            value, cursor = uleb(table, cursor)
            fields.append(value)
        sites.append(fields)
    before, protected, after = sites
    call, width, landing, action = protected
    assert width == 4 and action == 0 and call + width < landing < extent
    assert before == [0, call, 0, 0] and after == [call + width, extent - call - width, 0, 0]
    link, normal_branch = struct.unpack_from('<II', code, call)
    assert link & 0xfc000000 == 0x94000000 and normal_branch & 0xfc000000 == 0x14000000
    normal_delta = normal_branch & 0x03ffffff
    if normal_delta & 0x02000000:
        normal_delta -= 0x04000000
    normal = call + 4 + 4 * normal_delta
    assert call + 4 < normal < extent and normal != landing

    calls = {address - offset: name for address, kind, (name, _, _, _) in relocations(image, text)
             if offset <= address < offset + extent and kind == 0x2d}
    assert calls.pop(call) == '_may_throw'
    resume, = [address for address, name in calls.items() if name == '__Unwind_Resume']
    # Follow the branch and the LSDA rather than assuming their layout order.
    normal_end = landing if normal < landing else extent
    landing_end = normal if landing < normal else extent
    assert landing < resume < landing_end
    destructors = sorted(address for address, name in calls.items() if name.startswith('__ZN5GuardD'))
    assert len(destructors) == 2
    assert any(normal <= address < normal_end for address in destructors)
    assert any(landing <= address < resume for address in destructors)
    assert {'___cxa_begin_catch', '___cxa_end_catch'}.isdisjoint(name for name, _, _, _ in image.symbols)


def main():
    ycxx, *host = sys.argv[1:]
    # The same language fixtures as #663 exercise the two lifetimes and the original exception.
    sources = Path(__file__).resolve().parent / 'linux_local_unwind'
    env = dict(os.environ, LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='1')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='ycxx-darwin-local-unwind-', dir='.') as temporary:
        scope = Path(temporary).resolve()
        (scope / '.ycxx').touch()

        def run(*args):
            result = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=True, timeout=60)
            assert result.returncode == 0, (args, result)

        def compile(source, obj):
            run(ycxx, 'cxx', '--target=arm64-apple-darwin', '-c', source, '-o', obj)

        compile(sources / 'checked.cpp', 'checked.o')
        check_object(scope / 'checked.o')
        nonthrowing = scope / 'nonthrowing.cpp'
        nonthrowing.write_text((sources / 'checked.cpp').read_text().replace('void may_throw();',
                                                                           'void may_throw() noexcept;'))
        compile(nonthrowing, 'nonthrowing.o')
        assert (b'__TEXT', b'__gcc_except_tab') not in Image(scope / 'nonthrowing.o', kind=1).sections

        # PARITY: by design: hosted arm64 Mach-O execution requires macOS.
        if host:
            for optimization in ('-O0', '-O2'):
                run(host[0], '-std=c++20', '-pedantic-errors', '-fexceptions', '-fno-lto', optimization,
                    sources / 'peer.cpp', 'checked.o', '-o', 'hosted')
                run('./hosted')


if __name__ == '__main__':
    main()
