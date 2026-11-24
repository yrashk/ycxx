"""Link two x86-64 translation units that fold the same immediate specialization."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def check_symbols(readelf, artifact):
    symbols = subprocess.check_output(
        [readelf, "--wide", "--symbols", artifact], text=True
    )
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[7] == "_Z9calculateILi40EEii":
            raise SystemExit(f"{artifact.name} contains calculate<40>: {line.strip()}")


def main():
    ycxx, host_cxx, readelf = sys.argv[1:]
    source_dir = Path(__file__).with_suffix("")
    compile_command = [ycxx, "cxx", "--target=x86_64-pc-linux-gnu", "-c"]
    env = {**os.environ, "YCXX_NO_DAEMON": "1"}
    with tempfile.TemporaryDirectory(prefix="ycxx-immediate-template-") as directory:
        output = Path(directory)
        objects = []
        for source in ("main.cpp", "peer.cpp"):
            obj = output / Path(source).with_suffix(".o")
            subprocess.run(
                [*compile_command, source_dir / source, "-o", obj], env=env, check=True
            )
            objects.append(obj)

        for obj in objects:
            check_symbols(readelf, obj)
        executable = output / "immediate-template"
        subprocess.run([host_cxx, *objects, "-o", executable], check=True)
        check_symbols(readelf, executable)
        subprocess.run([executable], check=True)


if __name__ == "__main__":
    main()
