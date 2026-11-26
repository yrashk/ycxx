"""Measure cold and one-function incremental compiles on the division workload."""

import json
from pathlib import Path
import sys
from time import perf_counter_ns

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / "tests" / "daemon"))
from process import Env


def compile_once(world, env, mode):
    args = ["--target=x86_64-pc-linux-gnu", f"-fycxx-optimize={mode}", "-c", "input.cpp", "-o", "input.o"]
    start = perf_counter_ns()
    status, _, diagnostics = world.cxx(args, world.scope, env, timeout=30)
    elapsed = perf_counter_ns() - start
    if status:
        raise SystemExit(diagnostics.decode(errors="replace"))
    return elapsed


def measure(compiler, mode, repeat, source, edited):
    world = Env(compiler)
    world.cold["YCXX_THREADS"] = world.daemon["YCXX_THREADS"] = "1"
    try:
        path = world.scope / "input.cpp"
        object_file = world.scope / "input.o"
        path.write_text(source)
        cold = compile_once(world, world.cold, mode)
        original = object_file.read_bytes()
        compile_once(world, world.daemon, mode)
        if object_file.read_bytes() != original:
            raise SystemExit(f"{mode}: cold and daemon compiles disagree before the edit")
        path.write_text(edited)
        incremental = compile_once(world, world.daemon, mode)
        incremental_output = object_file.read_bytes()
        compile_once(world, world.cold, mode)
        if incremental_output == original or incremental_output != object_file.read_bytes():
            raise SystemExit(f"{mode}: incremental compile disagrees with a cold compile after the edit")
    finally:
        world.close()
    return {"repeat": repeat, "mode": mode, "cold_ns": cold, "incremental_ns": incremental}


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: compile-timings.py <ycxx>")
    compiler = str(Path(sys.argv[1]).resolve())
    source = (HERE / "workload.cpp").read_text()
    edit_at = " + 3 * "
    if edit_at not in source:
        raise SystemExit("workload changed before the pinned edit")
    edited = source.replace(edit_at, " + 5 * ", 1)
    modes = ("heuristic", "solver")
    for repeat in range(1, 11):
        for mode in modes if repeat % 2 else reversed(modes):
            print(json.dumps(measure(compiler, mode, repeat, source, edited)), flush=True)


if __name__ == "__main__":
    main()
