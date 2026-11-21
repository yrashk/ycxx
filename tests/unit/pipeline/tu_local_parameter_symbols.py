"""Check native binding, address use and cross-object calls for ordinary functions."""

import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


TARGETS = {"linux": "x86_64-pc-linux-gnu", "darwin": "arm64-apple-darwin"}


def elf_symbols(output):
    symbols = {}
    for line in output.splitlines():
        row = line.split()
        if len(row) == 8 and row[0].endswith(":") and row[4] in ("LOCAL", "GLOBAL", "WEAK"):
            symbols[row[7]] = (row[4], row[6] != "UND")
    return symbols


def macho_symbols(path):
    data = path.read_bytes()
    # The 64-bit arm64 MH_OBJECT header and LC_SYMTAB.
    assert struct.unpack_from("<III", data)[:2] == (0xFEEDFACF, 0x0100000C), path
    filetype, commands = struct.unpack_from("<II", data, 12)
    assert filetype == 1, path
    command_at = 32
    for _ in range(commands):
        kind, size = struct.unpack_from("<II", data, command_at)
        if kind == 2:
            offset, count, strings, string_size = struct.unpack_from("<IIII", data, command_at + 8)
            break
        command_at += size
    else:
        raise AssertionError(f"{path}: missing LC_SYMTAB")
    table = data[strings:strings + string_size]
    symbols = {}
    for name, kind, _, desc, _ in struct.iter_unpack("<IBBHQ", data[offset:offset + count * 16]):
        if kind & 0xE0:  # N_STAB (debug)
            continue
        binding = "WEAK" if desc & 0xC0 else "GLOBAL" if kind & 1 else "LOCAL"
        symbols[table[name:table.index(0, name)].decode()] = (binding, (kind & 0x0E) == 0x0E)
    return symbols


def main():
    ycxx, peer, host, *tools = sys.argv[1:]
    readelf = tools[0] if host == "linux" else None
    fixtures = Path(__file__).resolve().with_suffix("")
    with tempfile.TemporaryDirectory(prefix="ycxx-local-parameters-") as temporary:
        work = Path(temporary)
        (work / ".ycxx").touch()
        env = {k: v for k, v in os.environ.items() if k not in ("YCXX_DAEMON", "YCXX_ROOT")}
        env.update(LC_ALL="C", YCXX_NO_DAEMON="1", YCXX_THREADS="1")

        def run(*args, status=0):
            result = subprocess.run(args, cwd=work, env=env, capture_output=True, text=True, timeout=30)
            assert result.returncode == status, (args, result.returncode, result.stdout, result.stderr)
            return result

        def check_target(platform):
            target = f"--target={TARGETS[platform]}"
            prefix = "_" if platform == "darwin" else ""

            def cxx(*args, status=0):
                return run(ycxx, "cxx", target, "-std=c++26", *args, status=status)

            def compile_object(source):
                output = work / (source.removesuffix(".cpp").replace("/", "-") + ".o")
                cxx("-c", fixtures / source, "-o", output)
                return output

            def symbols(path):
                if platform == "linux":
                    return elf_symbols(run(readelf, "-Ws", path).stdout)
                return macho_symbols(path)

            def binding(path, name, kind, defined=True, *, platform_prefix=True):
                actual = symbols(path)
                symbol = prefix + name if platform_prefix else name
                assert actual.get(symbol) == (kind, defined), (platform, path, symbol, actual)

            def link_and_run(*objects):
                if platform == host:
                    output = work / "hosted"
                    run(peer, "-fno-lto", *objects, "-o", output)
                    run(output)

            ordinary = "_Z17ordinary_allocatemRKN12_GLOBAL__N_18LocalTagE"
            nested = ("_Z6choosePKN12_GLOBAL__N_15inner6RecordE",
                      "_Z6chooseN12_GLOBAL__N_15inner6Record4KindE")
            for group, mangled, driver in (("ordinary", (ordinary,), "driver"), ("nested", nested, "main")):
                sides = [compile_object(f"{group}/{side}.cpp") for side in ("left", "right")]
                for path in sides:
                    for name in mangled:
                        binding(path, name, "LOCAL")
                link_and_run(*sides, compile_object(f"{group}/{driver}.cpp"))

            address = compile_object("address/implementation.cpp")
            for name in ("_Z7primaryRKN12_GLOBAL__N_18LocalTagE", "_Z9alternateRKN12_GLOBAL__N_18LocalTagE"):
                binding(address, name, "LOCAL")
                if platform == "linux":
                    output = run(readelf, "-Wr", address).stdout
                    assert not any(name in line and "GOTPCREL" in line for line in output.splitlines()), output
            link_and_run(address, compile_object("address/main.cpp"))

            provider = compile_object("external/provider.cpp")
            consumer = compile_object("external/consumer.cpp")
            external = "_Z6chooseRKN6shared3TagE"
            binding(provider, external, "GLOBAL")
            binding(consumer, external, "GLOBAL", defined=False)
            link_and_run(consumer, provider)

            preserved = compile_object("preserve-scopes.cpp")
            binding(preserved, "_Z11return_onlyv", "GLOBAL")
            binding(preserved, "unaltered_c", "GLOBAL")
            binding(preserved, "fixture_preserved_asm_name", "GLOBAL", platform_prefix=False)
            link_and_run(preserved)

            missing = "_Z18missing_definitionRKN12_GLOBAL__N_18LocalTagE"
            binding(compile_object("completeness/other.cpp"), missing, "LOCAL")
            unused = compile_object("completeness/unused.cpp")
            assert prefix + missing not in symbols(unused), (platform, unused)
            used = fixtures / "completeness/used.cpp"
            cxx("-fsyntax-only", used)
            output = work / "absent.o"
            diagnostic = cxx("-c", used, "-o", output, status=1).stderr
            assert re.search(r"used\.cpp:2:\d+: error: function 'missing_definition' "
                             r"names a translation-unit-local type in a parameter and must be defined "
                             r"in this translation unit", diagnostic), diagnostic
            assert not output.exists(), (platform, diagnostic)

        check_target(host)
        if host == "linux":
            check_target("darwin")


if __name__ == "__main__":
    main()
