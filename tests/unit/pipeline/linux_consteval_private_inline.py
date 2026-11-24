"""Check native ELF emission of private constexpr helpers around if consteval."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def inspect(readelf, option, artifact):
    listing = subprocess.check_output([readelf, "--wide", option, artifact], text=True)
    return [line.split() for line in listing.splitlines()]


def check_compile_time_helper_absent(readelf, artifact):
    forbidden = {"_ZN6Buffer5cloneEm", "_Znam", "_ZdaPv", "_ZdaPvm"}
    for option in ("--symbols", "--relocs"):
        for fields in inspect(readelf, option, artifact):
            if forbidden.intersection(field.split("@", 1)[0] for field in fields):
                raise SystemExit(f"{artifact.name}: unexpected compile-time helper: {' '.join(fields)}")


def check_runtime_helper(readelf, artifact, name):
    symbols = inspect(readelf, "--symbols", artifact)
    if not any(len(row) >= 8 and row[7] == name and row[6] != "UND" for row in symbols):
        raise SystemExit(f"{artifact.name}: missing definition of {name}")
    relocations = inspect(readelf, "--relocs", artifact)
    if not any(len(row) >= 5 and row[4] == name for row in relocations):
        raise SystemExit(f"{artifact.name}: missing relocation to {name}")


def main():
    ycxx, host_cxx, readelf = sys.argv[1:]
    source_dir = Path(__file__).with_suffix("")
    compile_command = [ycxx, "cxx", "--target=x86_64-pc-linux-gnu", "-c"]
    env = {**os.environ, "YCXX_NO_DAEMON": "1"}
    with tempfile.TemporaryDirectory(prefix="ycxx-consteval-private-inline-") as directory:
        output = Path(directory)
        objects = [output / f"{source}.o" for source in ("allocating", "main", "peer")]
        for obj in objects:
            source = source_dir / obj.with_suffix(".cpp").name
            subprocess.run([*compile_command, source, "-o", obj], env=env, check=True)

        allocating, main_object, peer_object = objects
        check_compile_time_helper_absent(readelf, allocating)
        check_runtime_helper(readelf, main_object, "_ZN5Steps3addEi")
        for artifact in (main_object, peer_object):
            check_runtime_helper(readelf, artifact, "_ZN5Steps5twiceEi")

        executable = output / "consteval-private-inline"
        subprocess.run([host_cxx, *objects, "-o", executable], check=True)
        check_compile_time_helper_absent(readelf, executable)
        subprocess.run([executable], check=True)


if __name__ == "__main__":
    main()
