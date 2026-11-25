"""Pass the published shader bundle to the freestanding C++ example."""

import argparse
import os
from pathlib import Path


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binding", nargs="?", type=int, help="expected fragment binding")
parser.add_argument("--runtime-only", action="store_true")
parser.add_argument("--module", choices=["fragment", "vertex"], default="fragment")
args = parser.parse_args()

if args.runtime_only:
    if args.binding is not None:
        parser.error("omit binding with --runtime-only")
    target = "runtime_only"
    argument = args.module
else:
    if args.binding is None or not 0 <= args.binding <= 0xFFFFFFFF:
        parser.error("supply an unsigned 32-bit binding")
    if args.module != "fragment":
        parser.error("select the vertex module with --runtime-only")
    target = "app"
    argument = str(args.binding)

root = Path(__file__).resolve().parent / ".ycxx"
app = root / "out" / target
bundle = (root / "assets" / target / "scene.bundle").read_text(encoding="ascii")
os.execv(app, [str(app), argument, bundle])
