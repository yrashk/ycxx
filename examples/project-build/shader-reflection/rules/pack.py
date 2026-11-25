"""Pack staged SPIR-V as one named hexadecimal entry per line."""

import sys
from pathlib import Path


output, vertex, fragment = map(Path, sys.argv[1:])
with output.open("w", encoding="ascii", newline="\n") as bundle:
    bundle.write(f"vertex {vertex.read_bytes().hex()}\n")
    bundle.write(f"fragment {fragment.read_bytes().hex()}\n")
