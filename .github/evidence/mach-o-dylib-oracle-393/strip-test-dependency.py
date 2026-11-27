"""Remove only the unused libSystem load command from this oracle's Mach-O leaf."""

from pathlib import Path
import struct
import subprocess
import sys


image = Path(sys.argv[1])
undefined = subprocess.check_output(["xcrun", "nm", "-u", str(image)], text=True)
assert not undefined.strip(), undefined
data = bytearray(image.read_bytes())
magic, cpu, _, image_type, count, size, _, _ = struct.unpack_from("<8I", data)
assert (magic, cpu, image_type) == (0xFEEDFACF, 0x0100000C, 6), (magic, cpu, image_type)
pos, end = 32, 32 + size
kept, removed = [], []
for _ in range(count):
    kind, length = struct.unpack_from("<2I", data, pos)
    assert length >= 8 and length % 8 == 0 and pos + length <= end, (pos, length, end)
    command = data[pos:pos + length]
    if kind == 0xC:
        name_offset, = struct.unpack_from("<I", command, 8)
        name = command[name_offset:].partition(b"\0")[0].decode("utf-8")
        removed.append(name)
    else:
        assert kind not in (0x80000018, 0x8000001F, 0x20, 0x80000023), kind
        kept.append(command)
    pos += length
assert pos == end
assert removed == ["/usr/lib/libSystem.B.dylib"], removed
packed = b"".join(kept)
struct.pack_into("<2I", data, 16, len(kept), len(packed))
data[32:end] = packed.ljust(size, b"\0")
image.write_bytes(data)
print("Undefined symbols:", repr(undefined))
print("Removed:", removed)
print("Load command count:", count, "->", len(kept))
print("Load command bytes:", size, "->", len(packed))
