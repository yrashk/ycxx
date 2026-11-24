"""Check that the C library builtins call the C library's functions by one symbol.

The builtins are __builtin_memchr, memcmp, strchr, strpbrk, strrchr, strstr, memset, memcpy and
memmove (docs/EXTENSIONS.md).

Each fixture is compiled for both targets. The C function's symbol must appear exactly once in
the object's symbol table: undefined where the unit only declares it, defined where the unit
defines it. A second entry means the builtin's implicit declaration became a second entity (#248),
or, where a written declaration differs from it and they are two entities, that the object writer
gave each entity its own symbol (#1095). Entities the object defines keep a symbol each, so a weak
and a strong definition of one spelling list it twice (#694).
"""

import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

TARGETS = {"linux": "x86_64-pc-linux-gnu", "darwin": "arm64-apple-darwin"}


def elf_symbols(data):
    """(name, defined) for each ELF64 little-endian symbol table entry."""
    assert data[:4] == b"\x7fELF" and data[4] == 2 and data[5] == 1
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    out = []
    for _, kind, _, _, offset, size, link, _, _, entsize in sections:
        if kind != 2:  # SHT_SYMTAB
            continue
        strings = sections[link]
        table = data[strings[4]:strings[4] + strings[5]]
        for at in range(offset, offset + size, entsize):
            name, info, _, shndx, _, _ = struct.unpack_from("<IBBHQQ", data, at)
            if name:
                out.append((table[name:table.index(0, name)].decode(), shndx != 0))
    return out


def macho_symbols(data):
    """(name, defined) for each arm64 Mach-O symbol table entry."""
    assert struct.unpack_from("<II", data)[:2] == (0xFEEDFACF, 0x0100000C)
    _, commands = struct.unpack_from("<II", data, 12)
    at = 32
    for _ in range(commands):
        kind, size = struct.unpack_from("<II", data, at)
        if kind == 2:  # LC_SYMTAB
            offset, count, strings, string_size = struct.unpack_from("<IIII", data, at + 8)
            break
        at += size
    else:
        raise AssertionError("missing LC_SYMTAB")
    table = data[strings:strings + string_size]
    out = []
    for name, kind, _, _, _ in struct.iter_unpack("<IBBHQ", data[offset:offset + count * 16]):
        if name and not kind & 0xE0:
            out.append((table[name:table.index(0, name)].decode(), (kind & 0x0E) == 0x0E))
    return out


def main():
    ycxx = str(Path(sys.argv[1]).resolve())
    fixtures = Path(__file__).resolve().with_suffix("")
    library = ("memcmp", "memchr", "strchr", "strpbrk", "strrchr", "strstr", "memset", "memcpy", "memmove")
    expected = {
        "declared.cpp": dict.fromkeys(library, False),
        "defined.cpp": {"memcmp": True, "memcpy": True, "memset": True},
        "builtin-only.cpp": dict.fromkeys(library, False),
        "mismatched-declared.cpp": {"memcmp": False, "memchr": False},
        "mismatched-defined.cpp": {"memcmp": True},
        "same-spelling.cpp": {"_Z1fv": [True, True]},
    }
    failures = []
    with tempfile.TemporaryDirectory(prefix="ycxx-string-builtins-") as temporary:
        work = Path(temporary)
        (work / ".ycxx").touch()
        env = {k: v for k, v in os.environ.items() if k not in ("YCXX_DAEMON", "YCXX_ROOT")}
        env.update(LC_ALL="C", YCXX_NO_DAEMON="1")
        for host, triple in TARGETS.items():
            for fixture, symbols in expected.items():
                obj = work / f"{host}-{fixture}.o"
                result = subprocess.run([ycxx, "cxx", f"--target={triple}", "-std=c++26", "-c", str(fixtures / fixture),
                                         "-o", str(obj)], cwd=work, env=env, capture_output=True, text=True, timeout=60)
                if result.returncode != 0:
                    failures.append(f"{host} {fixture}: exit {result.returncode}\n{result.stderr}")
                    continue
                data = obj.read_bytes()
                table = elf_symbols(data) if host == "linux" else macho_symbols(data)
                prefix = "" if host == "linux" else "_"
                for name, defined in symbols.items():
                    entries = [d for n, d in table if n == prefix + name]
                    expected_entries = defined if isinstance(defined, list) else [defined]
                    if entries != expected_entries:
                        failures.append(f"{host} {fixture}: {prefix + name} entries {entries}, expected {expected_entries}")
    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
