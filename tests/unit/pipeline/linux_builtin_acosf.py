"""Check the float library call in ELF and run it against a separate provider."""

import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run


def main():
    ycxx, host_cxx, readelf = sys.argv[1:]
    fixtures = Path(__file__).resolve().with_suffix("")
    env = dict(os.environ, LC_ALL="C")
    with tempfile.TemporaryDirectory(prefix="ycxx-builtin-acosf-") as directory:
        work = Path(directory)
        run([ycxx, "cxx", "--no-daemon", "--target=x86_64-linux-gnu", "-std=c++26",
             "-c", fixtures / "runtime.cpp", "-o", "runtime.o"], work, env)

        table = run([readelf, "--wide", "--symbols", "--relocs", "runtime.o"], work, env)
        rows = [line.split() for line in table.splitlines()]
        undefined = [row[7] for row in rows if len(row) >= 8 and row[6] == "UND"]
        assert undefined == ["acosf"], table
        assert any(len(row) >= 5 and row[2].startswith("R_X86_64_") and row[4] == "acosf"
                   for row in rows), table

        run([host_cxx, "-std=c++26", "-fno-builtin-acosf", "-no-pie",
             fixtures / "provider.cpp", "runtime.o", "-o", "mixed"], work, env)
        run(["./mixed"], work, env)


if __name__ == "__main__":
    main()
