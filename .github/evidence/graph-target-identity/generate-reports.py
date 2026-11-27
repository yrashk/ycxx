#!/usr/bin/env python3
"""Export actual reports from the pinned #1611 daemon test fixtures."""

import argparse
import json
from pathlib import Path
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args()
    driver = options.driver.resolve(strict=True)
    fixtures = (options.repo / "tests" / "daemon").resolve(strict=True)
    out = options.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    sys.path.insert(0, str(fixtures))
    from project_build import env_for, run_client
    from project_graph_target_identity import (
        check_identity, check_partial, identity_report, partial_component_report,
    )
    from project_inspection import scope_state
    from runtime_dir import runtime_root

    cases = (
        ("identity", identity_report, check_identity),
        ("partial", partial_component_report, check_partial),
    )
    with tempfile.TemporaryDirectory(prefix="ycxx-graph-reports-") as workspace:
        for name, prepare, check in cases:
            scope = Path(workspace).resolve() / name
            with tempfile.TemporaryDirectory(prefix="yb", dir=runtime_root("yb")) as runtime:
                env = env_for(runtime)
                try:
                    plan, document = prepare(driver, scope, env)
                    check(plan, document)
                    # The fixture returns decoded JSON. Fetch the emitted JSON
                    # text and check that it still describes the same plan.
                    before = scope_state(scope)
                    raw = run_client(driver, scope, env, "show", "app", "--format=json")
                    assert raw.returncode == 0, raw.stderr
                    assert json.loads(raw.stdout) == plan, name
                    assert scope_state(scope) == before, f"{name} JSON show changed the workspace"
                    (out / f"{name}.html").write_bytes(document.encode("utf-8"))
                    (out / f"{name}.json").write_bytes(raw.stdout.encode("utf-8"))
                finally:
                    if scope.is_dir():
                        run_client(driver, scope, env, "ctl", "shutdown")


if __name__ == "__main__":
    main()
