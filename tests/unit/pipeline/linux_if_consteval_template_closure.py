"""Check ELF emission for template-local if consteval closures and a scalar runtime demand."""

from pathlib import Path
import subprocess
import sys
import tempfile


def check_definition(readelf, artifact, name):
    symbols = subprocess.check_output([readelf, "--wide", "--symbols", artifact], text=True)
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[3] == "FUNC" and fields[6] != "UND" and fields[7] == name:
            return
    raise SystemExit(f"{artifact.name}: missing definition of {name}")


def main():
    ycxx, host_cxx, readelf, fixtures = sys.argv[1:]
    fixture_dir = Path(fixtures)
    compile_command = [ycxx, "cxx", "--no-daemon", "--target=x86_64-linux-gnu", "-std=c++26", "-c"]
    cases = {
        "ordinary-instantiated.accept.cpp": "ordinary_entry",
        "pattern-only.accept.cpp": "pattern_anchor",
        "negated-instantiated.accept.cpp": "negated_entry",
        "dormant-helper.accept.cpp": "deferred_pattern_anchor",
        "runtime-helper.run.cpp": "main",
    }

    with tempfile.TemporaryDirectory(prefix="ycxx-if-consteval-template-closure-") as directory:
        output = Path(directory)
        for fixture, anchor in cases.items():
            artifact = output / f"{anchor}.o"
            subprocess.run([*compile_command, fixture_dir / fixture, "-o", artifact], check=True)
            check_definition(readelf, artifact, anchor)

        executable = output / "runtime-helper"
        subprocess.run([host_cxx, output / "main.o", "-o", executable], check=True)
        subprocess.run([executable], check=True)


if __name__ == "__main__":
    main()
