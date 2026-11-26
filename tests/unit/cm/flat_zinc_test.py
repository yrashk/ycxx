"""Check generated FlatZinc with the offline Chuffed executable."""

import subprocess
import sys


def main():
    fixture, solver = sys.argv[1:]
    cases = {
        # (0, 4) costs -12. The feasible (1, 2), (1, 3), and (1, 4) cost -1, -4, and -7.
        "minimum": ("x0 = 0;", "x1 = 4;", "----------", "=========="),
        "unsat": ("=====UNSATISFIABLE=====",),
        "constant": ("----------", "=========="),
        "satisfy": ("x0 = -1;", "----------"),
    }
    for case, expected in cases.items():
        model = subprocess.check_output([fixture, "--emit", case], text=True, timeout=10)
        solved = subprocess.run(
            [solver, "--rnd-seed", "1"],
            input=model,
            text=True,
            capture_output=True,
            timeout=10,
        )
        lines = [line for line in map(str.strip, solved.stdout.splitlines()) if line and not line.startswith("%")]
        # An optimization run may emit earlier incumbents. Check the final assignment and status.
        if solved.returncode or tuple(lines[-len(expected):]) != expected:
            raise SystemExit(
                f"{case} failed with exit code {solved.returncode}\n"
                f"Model:\n{model}\nSolver output:\n{solved.stdout}\n{solved.stderr}"
            )


if __name__ == "__main__":
    main()
