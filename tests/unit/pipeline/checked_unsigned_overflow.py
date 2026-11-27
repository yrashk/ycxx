"""Check native checked-unsigned arithmetic in both object formats and on Linux."""

import os
from pathlib import Path
import platform
import sys
import tempfile

from linux_compile import run
from string_builtin_symbols import TARGETS, elf_symbols, macho_symbols


def main():
    ycxx, = sys.argv[1:]
    ycxx = str(Path(ycxx).resolve())
    source = Path(__file__).resolve().with_suffix("") / "checked-unsigned-runtime.cpp"
    env = dict(os.environ, LC_ALL="C")
    driver = [ycxx, "cxx", "--no-daemon", "-std=c++26", "-ffreestanding"]
    entry = "checked_overflow_runtime"
    with tempfile.TemporaryDirectory(prefix="ycxx-checked-overflow-") as directory:
        work = Path(directory)
        for host, target in TARGETS.items():
            artifact = work / f"{host}.o"
            run([*driver, f"--target={target}", "-c", source, "-o", artifact], work, env)
            data = artifact.read_bytes()
            table = elf_symbols(data) if host == "linux" else macho_symbols(data)
            symbol = entry if host == "linux" else "_" + entry
            assert [defined for name, defined in table if name == symbol] == [True], (target, table)

        # PARITY: by design: the freestanding x86-64 ELF executable runs on a Linux x86-64 host.
        if sys.platform == "linux" and platform.machine().lower() in ("x86_64", "amd64"):
            (work / "main.cpp").write_text(f'extern "C" int {entry}();\nint main() {{ return {entry}(); }}\n')
            run([*driver, f"--target={TARGETS['linux']}", "-nostdlib",
                 "main.cpp", "linux.o", "-o", "checked-overflow"], work, env)
            run(["./checked-overflow"], work, env)


if __name__ == "__main__":
    main()
