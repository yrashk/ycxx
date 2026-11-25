"""Pass the published shader bundle to the freestanding C++ example."""

import os
import sys
from pathlib import Path


if len(sys.argv) > 2:
    raise SystemExit("usage: python3 run.py [shader-name]")

root = Path(__file__).resolve().parent / ".ycxx"
app = root / "out" / "app"
name = sys.argv[1] if len(sys.argv) == 2 else "triangle-fragment"
bundle = (root / "assets" / "app" / "scene.bundle").read_text(encoding="ascii")
os.execv(app, [str(app), name, bundle])
