"""Check generated FlatZinc with the offline Chuffed executable."""

import subprocess
import sys


def main():
    fixture, solver = sys.argv[1:]
    cases = {
        "minimum": ("x0 = 1;", "x1 = 2;", "----------", "=========="),
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
        lines = solved.stdout.splitlines()
        if solved.returncode or not all(line in lines for line in expected):
            raise SystemExit(
                f"{case} failed with exit code {solved.returncode}\n"
                f"Model:\n{model}\nSolver output:\n{solved.stdout}\n{solved.stderr}"
            )


if __name__ == "__main__":
    main()
