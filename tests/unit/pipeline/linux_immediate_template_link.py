"""Check native linking after immediate calls fold."""

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



def check_checked_specializations(readelf, artifact):
    symbols = subprocess.check_output(
        [readelf, "--wide", "--symbols", artifact], text=True
    )
    entries = [line.split() for line in symbols.splitlines()]
    entries = [fields for fields in entries if len(fields) >= 8]
    omitted = "_Z11checked_sumILi1EEbl"
    retained = "_Z11checked_sumILi2EEbl"
    if any(fields[7] == omitted for fields in entries):
        raise SystemExit(f"{artifact.name} contains unreferenced checked_sum<1>")
    if not any(fields[7] == retained and fields[3] == "FUNC" and fields[6].isdecimal()
               for fields in entries):
        raise SystemExit(f"{artifact.name} has no definition of retained checked_sum<2>")
    relocations = subprocess.check_output(
        [readelf, "--wide", "--relocs", artifact], text=True
    )
    if retained not in relocations or "checked_input" in relocations:
        raise SystemExit(f"unexpected checked specialization relocations in {artifact.name}")

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

        retained = output / "retained-caller"
        retained_obj = retained.with_suffix(".o")
        subprocess.run(
            [*compile_command, source_dir / "retained-caller.cpp", "-o", retained_obj],
            env=env,
            check=True,
        )
        subprocess.run([host_cxx, retained_obj, "-o", retained], check=True)
        subprocess.run([retained], check=True)

        checked = output / "retained-checked-caller"
        checked_obj = checked.with_suffix(".o")
        subprocess.run(
            [*compile_command, source_dir / "retained-checked-caller.cpp", "-o", checked_obj],
            env=env,
            check=True,
        )
        check_checked_specializations(readelf, checked_obj)
        subprocess.run([host_cxx, checked_obj, "-o", checked], check=True)
        subprocess.run([checked], check=True)


if __name__ == "__main__":
    main()
