#!/usr/bin/env python3
"""Capture source globs and named artifacts in the native project example (#1253)."""

import argparse
import importlib.util
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile

GLOB = 'glob("src/*.cc", exclude = ["src/*_test.cc"])'
ARTIFACT = 'artifact("input_header", "header")'
HEADER = ".ycxx/virtual/input_header/input_value.h"
APP = Path(".ycxx/out/app")
MAIN = "src/main.cc"
ADDED = ("src/a_glob_probe.cc", "src/z_glob_probe.cc")
EXCLUDED = ("src/twice_test.cc", "src/glob_probe_test.cc")
POISON = "#error an excluded source must not compile\n"


def browser_evidence(output, manifest, source_line, header_line):
    from playwright.sync_api import expect, sync_playwright

    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(
            executable_path=shutil.which("chromium"),
            args=["--no-sandbox", "--disable-dev-shm-usage", "--disable-background-networking"],
        )
        try:
            context = browser.new_context(
                offline=True, service_workers="block", viewport={"width": 1440, "height": 900},
            )
            context.route(re.compile(r"^https?://"), lambda route: route.abort())
            page = context.new_page()
            requests, errors = [], []
            page.on("request", lambda request: requests.append(request.url))
            page.on("websocket", lambda socket: requests.append(socket.url))
            page.on("pageerror", lambda error: errors.append(str(error)))
            page.goto((output / "added.html").as_uri(), wait_until="load")

            graph = page.locator("#target-graph")
            details = graph.locator("#graph-details")
            expect(graph).to_be_visible()
            source_facts = details.locator(".graph-fact").filter(has=page.get_by_text(GLOB, exact=True))
            expect(source_facts.locator(".fact-value")).to_have_text([ADDED[0], MAIN, ADDED[1]])
            for name in EXCLUDED:
                expect(details).not_to_contain_text(name)
            source_link = source_facts.first.locator("a")
            expect(source_link).to_have_attribute("href", f"#manifest-line-{source_line}")
            expect(page.locator(f"#manifest-line-{source_line}")).to_have_text(manifest.splitlines()[source_line - 1])

            edge = details.locator(".graph-relation").filter(has_text="input_header")
            expect(edge).to_contain_text("generated_headers")
            expect(edge).to_contain_text("Output header")
            expect(edge.locator(f"a[href='#manifest-line-{header_line}']")).to_have_count(1)
            graph.screenshot(path=output / "addition-graph.png", animations="disabled")
            source_link.press("Enter")
            assert page.url.endswith(f"#manifest-line-{source_line}"), page.url

            graph.get_by_role("button", name="input_header", exact=True).press("Enter")
            expect(details.locator(".graph-fact").filter(has_text=HEADER)).to_contain_text("Output")
            graph.screenshot(path=output / "named-header-graph.png", animations="disabled")

            page.goto((output / "deleted-diff.html").as_uri(), wait_until="load")
            changes = page.locator("section[aria-labelledby='changes']")
            removal = changes.locator("li").filter(has_text=ADDED[1])
            expect(removal).to_have_count(1)
            expect(removal).to_contain_text("Removed")
            expect(page.locator("#graph-details")).not_to_contain_text(ADDED[1])
            changes.screenshot(path=output / "deletion-plan-diff.png", animations="disabled")

            external = [url for url in requests if re.match(r"^(https?|wss?)://", url)]
            assert not errors and not external, (errors, external)
        finally:
            browser.close()


def report(driver, scope, env, output, label, expected_sources, build, inspection):
    before = inspection.scope_state(scope)
    results = {}
    for kind in ("json", "html"):
        done = build.run_client(driver, scope, env, "show", "app", f"--format={kind}")
        assert done.returncode == 0, done.stderr
        (output / f"{label}.{kind}").write_text(done.stdout, encoding="utf-8")
        results[kind] = done.stdout

    assert inspection.scope_state(scope) == before, "show changed the workspace"
    plan = json.loads(results["json"])
    definition = (scope / "build.ycxx").read_text(encoding="utf-8")
    assert plan["schema"] == 1 and plan["target"] == "app" and plan["definition"] == definition
    sources = [row for row in plan["facts"] if row["kind"] == "source" and row["target"] == "app"]
    assert [row["value"] for row in sources] == expected_sources, sources
    for row in sources:
        assert row["origin"] == GLOB, row
        inspection.check_location(row, definition, GLOB)

    header = inspection.fact(plan, "output", "input_header", HEADER)
    assert header["name"] == "header"
    header_edge = inspection.dependency_edge(
        definition, "app", "input_header", "generated_headers", ARTIFACT, output="header",
    )
    library_edge = inspection.dependency_edge(
        definition, "app", "arithmetic", "deps", '"arithmetic"', start=definition.index("deps = ["),
    )
    inspection.check_dependency_graph(plan, [library_edge, header_edge])
    inspection.check_target_graph_html(results["html"], plan)
    return definition, sources[0]["line"], header_edge["location"]["line"]


def watch_evidence(driver, scope, env, output, build, watch_tools, inspection):
    installed = scope / APP

    def no_excluded(events):
        for name in EXCLUDED:
            assert not build.tu_outcomes(events, Path(name).name), name

    with (output / "watch.jsonl").open("w", encoding="utf-8") as trace:
        class EvidenceWatch(watch_tools.Watch):
            def record(self, deadline):
                record = super().record(deadline)
                trace.write(json.dumps(record) + "\n")
                trace.flush()
                return record

        watch = EvidenceWatch(driver, scope, env)
        try:
            summary, initial, _ = watch.build("initial native example")
            build.check_program(installed, 42)
            assert build.action_keys(initial, "ExternalAction") == ["input_header"], summary
            assert "executed" in build.tu_outcomes(initial, "main.cc")
            no_excluded(initial)
            report(driver, scope, env, output, "initial", [MAIN], build, inspection)

            for name in EXCLUDED:
                watch_tools.replace_source(scope, name, POISON)
            for name, symbol in zip(reversed(ADDED), ("last", "first")):
                watch_tools.replace_source(scope, name, f"int glob_probe_{symbol}() {{ return 1; }}\n")

            def observed(label, present, missing=()):
                # Excluded entries may cause a directory reobservation or an extra build.
                events = []
                for _ in range(12):
                    summary, cycle, watching = watch.build(label)
                    no_excluded(cycle)
                    events.extend(cycle)
                    paths = {(scope / path).resolve() for path in watching}
                    if all((scope / path).resolve() in paths for path in present) and all(
                        (scope / path).resolve() not in paths for path in missing
                    ):
                        assert summary["actions_executed"] == 0, summary
                        return events, cycle
                raise AssertionError((label, "watch never reached expected membership", watching))

            added, _ = observed("new glob matches", ADDED)
            for name in ADDED:
                assert "executed" in build.tu_outcomes(added, Path(name).name), name
            build.check_program(installed, 42)
            plan = report(driver, scope, env, output, "added", [ADDED[0], MAIN, ADDED[1]], build, inspection)

            (scope / ADDED[1]).unlink()
            _, deletion_cycle = observed("deleted glob match", (ADDED[0],), (ADDED[1],))
            assert not build.tu_outcomes(deletion_cycle, Path(ADDED[1]).name)
            build.check_program(installed, 42)
            report(driver, scope, env, output, "deleted", [ADDED[0], MAIN], build, inspection)

            before = inspection.scope_state(scope)
            diff = build.run_client(driver, scope, env, "show", "app", "--format=html",
                                    "--against", str(output / "added.json"))
            assert diff.returncode == 0, diff.stderr
            assert inspection.scope_state(scope) == before, "plan comparison changed the workspace"
            (output / "deleted-diff.html").write_text(diff.stdout, encoding="utf-8")
            return plan
        finally:
            watch.close()


def interactive_evidence(driver, example, scope, env, output, helper, build, watch_tools):
    shutil.copytree(example, scope, ignore=shutil.ignore_patterns(".ycxx", ".git"))
    spec = importlib.util.spec_from_file_location("terminal_capture", helper)
    terminal = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(terminal)
    extra = "src/live_glob_probe.cc"
    for name in EXCLUDED:
        (scope / name).write_text(POISON, encoding="utf-8")
    try:
        terminal.terminal_trace(
            driver, scope, env, output / "interactive-new-match", (),
            "kind:Tu live_glob_probe.cc is:executed", "live_glob_probe.cc)",
            lambda: watch_tools.replace_source(scope, extra, "int live_glob_probe() { return 1; }\n"),
            lambda: terminal.app_status(scope, scope / APP) == 42,
        )
    finally:
        build.run_client(driver, scope, env, "ctl", "shutdown")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    parser.add_argument("repository", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--browser", action="store_true")
    parser.add_argument("--terminal-helper", type=Path)
    args = parser.parse_args()
    driver, repo, output = [value.resolve() for value in (args.driver, args.repository, args.output)]
    example = repo / "examples/project-build/native"
    output.mkdir(parents=True, exist_ok=True)

    sys.path.insert(0, str(repo / "tests/daemon"))
    import project_build as build
    import project_build_watch as watch_tools
    import project_inspection as inspection
    from runtime_dir import runtime_root

    with (
        tempfile.TemporaryDirectory(prefix="ycxx-native-glob-") as work,
        tempfile.TemporaryDirectory(prefix="yg", dir=runtime_root("yg")) as runtime,
    ):
        copied = Path(work) / "json-watch"
        shutil.copytree(example, copied, ignore=shutil.ignore_patterns(".ycxx", ".git"))
        env = build.env_for(runtime)
        env["TERM"] = "xterm-256color"
        try:
            manifest, source_line, header_line = watch_evidence(
                driver, copied, env, output, build, watch_tools, inspection,
            )
        finally:
            build.run_client(driver, copied, env, "ctl", "shutdown")
        if args.browser:
            browser_evidence(output, manifest, source_line, header_line)
        if args.terminal_helper:
            interactive_evidence(driver, example, Path(work) / "interactive-watch", env, output,
                                 args.terminal_helper.resolve(), build, watch_tools)

    print("Native example: app exited 42, added glob matches compiled, a deletion removed a match, "
          "excluded sources stayed out, and the named header kept its manifest site.")


if __name__ == "__main__":
    main()
