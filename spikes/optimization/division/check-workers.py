"""Replay the same solver workload at one and four workers and retain the traces."""

import itertools
import json
import os
from pathlib import Path
import subprocess
import sys


def source():
    for index in range(10):
        terms = ["((x / d) + (x % d))"] * (140 + 7 * index)
        name = f"work_{index}"
        yield f"unsigned long long {name}(unsigned long long x, unsigned long long d) {{\n"
        yield "  return " + " +\n    ".join(terms) + f" + {index}ULL;\n}}\n\n"


def compile_and_trace(compiler, work, workers, trial):
    name = f"workers-{workers}-{trial}"
    obj = work / f"{name}.o"
    trace = work / f"{name}.ndjson"
    with trace.open("wb") as stream:
        subprocess.run(
            [compiler, "cxx", "--target=x86_64-pc-linux-gnu", "-fycxx-optimize=solver",
             "--trace-engine=json", "-c", str(work / "parallel.cpp"), "-o", str(obj)],
            env=dict(os.environ, YCXX_NO_DAEMON="1", YCXX_THREADS=str(workers)),
            stderr=stream, timeout=120, check=True,
        )

    events = [json.loads(line) for line in trace.read_text().splitlines()]
    solves = [e for e in events if e.get("kind") == "DivisionFusionSolve" and e["outcome"] == "executed"]
    if len(solves) != 10:
        raise SystemExit(f"{trace}: expected ten independent solves")
    intervals = []
    for solve in solves:
        models = [e for e in events if e.get("parent") == solve["id"] and e.get("kind") == "DivisionFusionModel"]
        if (solve["worker"] != solve["end_worker"] or "outcome=optimal" not in solve["value"]
                or len(models) != 1 or models[0]["outcome"] != "reused"):
            raise SystemExit(f"{trace}: solve {solve['id']} did not finish on its worker with a ready model")
        model = models[0]
        start = max(solve["start_ns"], model["start_ns"] + model["duration_ns"])
        intervals.append((start, solve["start_ns"] + solve["duration_ns"], solve["worker"]))
    overlap = max(
        (min(end, other_end) - max(start, other_start)
         for (start, end, worker), (other_start, other_end, other) in itertools.combinations(intervals, 2)
         if worker != other),
        default=0,
    )
    print(f"{trace}: workers {sorted({worker for _, _, worker in intervals})}, "
          f"longest overlap {max(0, overlap)} ns")
    return obj.read_bytes(), overlap


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: check-workers.py <ycxx> <output-directory>")
    compiler, work = (Path(arg).resolve() for arg in sys.argv[1:])
    work.mkdir()
    (work / "parallel.cpp").write_text("".join(source()))
    reference, _ = compile_and_trace(compiler, work, 1, 1)
    observed = False
    for trial in range(1, 4):
        obj, overlap = compile_and_trace(compiler, work, 4, trial)
        if obj != reference:
            raise SystemExit(f"trial {trial}: object bytes changed with worker count")
        observed |= overlap > 0
    if not observed:
        raise SystemExit("byte-identical objects, with no observed overlap in these captures")
    print("byte-identical objects with concurrent solving")


if __name__ == "__main__":
    main()
