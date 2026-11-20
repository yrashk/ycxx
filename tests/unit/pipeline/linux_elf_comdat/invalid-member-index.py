"""Set one COMDAT member index to the first invalid ELF section index."""

import struct
import sys
from pathlib import Path

source = Path(sys.argv[1]).read_bytes()
if source[:6] != b"\x7fELF\x02\x01":
    raise SystemExit("expected little-endian ELF64")

section_table_offset, = struct.unpack_from("<Q", source, 40)
section_header_size, section_count = struct.unpack_from("<HH", source, 58)
if section_header_size != 64 or not section_count:
    raise SystemExit("expected an ordinary ELF64 section table")

group_offsets = []
for index in range(section_count):
    header_offset = section_table_offset + index * section_header_size
    section_type, = struct.unpack_from("<I", source, header_offset + 4)
    if section_type == 17:  # SHT_GROUP
        group_offsets.append(struct.unpack_from("<Q", source, header_offset + 24)[0])

if len(group_offsets) != 1:
    raise SystemExit("expected one section group")
group_offset, = group_offsets
flags, first_member = struct.unpack_from("<II", source, group_offset)
if flags != 1 or not 0 < first_member < section_count:
    raise SystemExit("expected a COMDAT group with a valid first member")

mutated = bytearray(source)
struct.pack_into("<I", mutated, group_offset + 4, section_count)
Path(sys.argv[2]).write_bytes(mutated)
