"""Change the original group-b.o signature to its existing from_b symbol."""

import struct
import sys
from pathlib import Path

SHT_GROUP = 17
SHT_SYMTAB = 2
SHF_GROUP = 0x200
STB_GLOBAL = 1

source = Path(sys.argv[1]).read_bytes()
if source[:6] != b"\x7fELF\x02\x01":
    raise SystemExit("expected little-endian ELF64")

section_table_offset, = struct.unpack_from("<Q", source, 40)
section_header_size, section_count = struct.unpack_from("<HH", source, 58)
if section_header_size != 64 or not section_count:
    raise SystemExit("expected an ordinary ELF64 section table")
sections = [
    struct.unpack_from("<IIQQQQIIQQ", source, section_table_offset + index * 64)
    for index in range(section_count)
]

def string_at(offset):
    return source[offset:source.index(b"\0", offset)]

changed = None
for index, section in enumerate(sections):
    if section[1] != SHT_GROUP:
        continue

    symtab = sections[section[6]]
    if symtab[1] != SHT_SYMTAB or symtab[9] != 24:
        raise SystemExit("expected ordinary ELF64 group symbols")
    strtab_offset = sections[symtab[6]][4]
    symbols = [
        struct.unpack_from("<IBBHQQ", source, offset)
        for offset in range(symtab[4], symtab[4] + symtab[5], symtab[9])
    ]
    if string_at(strtab_offset + symbols[section[7]][0]) != b"_Z12select_groupv":
        continue

    replacements = [
        symbol_index
        for symbol_index, (name, info, _, section_index, *_) in enumerate(symbols)
        if string_at(strtab_offset + name) == b"from_b"
        and info >> 4 == STB_GLOBAL
        and 0 < section_index < section_count
        and not sections[section_index][2] & SHF_GROUP
    ]
    if len(replacements) != 1 or changed is not None:
        raise SystemExit("expected one common group and one nongroup from_b symbol")
    replacement, = replacements
    sh_info_offset = section_table_offset + index * section_header_size + 44
    changed = bytearray(source)
    struct.pack_into("<I", changed, sh_info_offset, replacement)
    print(f"group [{index}] sh_info at file offset {sh_info_offset}: "
          f"{section[7]} -> {replacement}")

if changed is None:
    raise SystemExit("common group signature not found")
Path(sys.argv[2]).write_bytes(changed)
