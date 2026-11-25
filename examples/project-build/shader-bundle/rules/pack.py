"""Pack staged SPIR-V as one named hexadecimal entry per line."""

import sys
from pathlib import Path


output, *entries = sys.argv[1:]
with Path(output).open("w", encoding="ascii", newline="\n") as bundle:
    for index in range(0, len(entries), 2):
        name, path = entries[index:index + 2]
        bundle.write(f"{name} {Path(path).read_bytes().hex()}\n")
