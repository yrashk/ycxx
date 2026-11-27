#!/usr/bin/env python3
"""Witness named-output cutoff and failed publication using the pinned daemon fixture."""

import argparse
from hashlib import sha256
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile

APP = Path(".ycxx/out/app")
ASSETS = Path(".ycxx/assets/app")
VIRTUAL = ".ycxx/virtual/wire"
SOURCE = "artifact(\"wire\", \"source\")"
HEADER = "artifact(\"wire\", \"header\")"
HEADER_BODY = b"inline int header_value() { return 5; }\n"
PARTIAL_BODY = b"partial\n"
GREEN = {"reused", "verified", "durable"}


def prepare(scope, build):
    build.prepare(scope)
    manifest = scope / "build.ycxx"
    plain = manifest.read_text(encoding="utf-8")
    final_field = '    deps = ["core"]\n'
    assert plain.count(final_field) == 1
    # Publish the exact same wire outputs as runtime assets so their bytes can be checked.
    definition = plain.replace(final_field, final_field + f"    runtime_assets = [{SOURCE}, {HEADER}]\n")
    manifest.write_text(definition, encoding="utf-8")
    return definition


def source_body(value):
    return f"int wire() {{ return {value}; }}\n".encode("ascii")


def provenance(driver, scope, env, output, label, definition, build, inspection):
    before = inspection.scope_state(scope)
    snapshots = {}
    for kind in ("json", "html"):
        done = build.run_client(driver, scope, env, "show", "app", f"--format={kind}")
        assert done.returncode == 0, done.stderr
        snapshots[kind] = done.stdout
        (output / f"{label}-plan.{kind}").write_text(done.stdout, encoding="utf-8")
    assert inspection.scope_state(scope) == before, "show changed the workspace"

    plan = json.loads(snapshots["json"])
    assert plan["schema"] == 1 and plan["target"] == "app" and plan["definition"] == definition
    edge = inspection.dependency_edge
    edges = [
        edge(definition, "app", "core", "deps", '"core"', start=definition.index("deps = [")),
        edge(definition, "app", "wire", "sources", SOURCE, output="source"),
        edge(definition, "app", "wire", "generated_headers", HEADER, output="header"),
    ]
    runtime_field = definition.index("runtime_assets =")
    for name, filename, binding in (("source", "wire.cc", SOURCE), ("header", "wire.h", HEADER)):
        edges.append(edge(definition, "app", "wire", "runtime_assets", binding, start=runtime_field, output=name))
        output_fact = inspection.fact(plan, "output", "wire", f"{VIRTUAL}/{filename}")
        assert output_fact["name"] == name
        inspection.check_location(output_fact, definition, f'"{filename}"')
        asset = inspection.fact(plan, "runtime-asset", "app", f"{ASSETS}/{filename}")
        assert asset["origin"] == binding, asset
        site = edges[-1]["location"]
        assert (asset["line"], asset["column"]) == (site["line"], site["column"]), asset
    inspection.check_dependency_graph(plan, edges)
    inspection.check_target_dependencies_html(snapshots["html"], plan)
    inspection.check_target_graph_html(snapshots["html"], plan)
    return edges


def publication(scope, output, label, value, check_assets):
    expected = {"wire.cc": source_body(value), "wire.h": HEADER_BODY}
    asset_stamps = check_assets(scope / ASSETS, expected)
    path = scope / APP
    image = path.read_bytes()
    stat = path.stat()
    app_stamp = (stat.st_dev, stat.st_ino, stat.st_mtime_ns)
    payloads = output / "payloads" / label
    payloads.mkdir(parents=True, exist_ok=True)
    hashes = {}
    for name in expected:
        payload = (scope / ASSETS / name).read_bytes()
        (payloads / name).write_bytes(payload)
        hashes[name] = sha256(payload).hexdigest()
    return hashes | {"app": sha256(image).hexdigest()}, image, app_stamp, asset_stamps


def receipt_events(records):
    parents = {event["id"] for event in records if event.get("kind") == "WorkspaceBuild"}
    return [event for event in records if event.get("kind") == "ActionReceipt"
            and event.get("parent") in parents]


def generated_events(records, name):
    return [event for event in records if event.get("kind") == "GeneratedFile"
            and Path(event.get("key", "")).name == name]


def observed_size(events, size):
    return any(event.get("value") == f"{size} bytes" for event in events)


def watch_evidence(driver, scope, env, output, definition, build, watches, inspection, check_assets):
    log = (output / "watch.jsonl").open("w", encoding="utf-8")

    class EvidenceWatch(watches.Watch):
        def record(self, deadline):
            row = super().record(deadline)
            log.write(json.dumps(row, ensure_ascii=False) + "\n")
            log.flush()
            return row

    watch = EvidenceWatch(driver, scope, env)
    try:
        app = scope / APP
        summary, initial, _ = watch.build("successful initial named outputs")
        build.check_cold_actions(summary, initial)
        build.check_program(app, 17)
        before = publication(scope, output, "initial", 7, check_assets)

        watches.replace_source(scope, "schema.txt", "11\n")
        summary, updated, _ = watch.build("source-only output edit")
        build.check_wire_action(summary, updated)
        build.check_program(app, 21)
        assert "executed" in build.tu_outcomes(updated, "wire.cc")
        main_outcomes = build.tu_outcomes(updated, "main.cc")
        assert main_outcomes and main_outcomes <= GREEN, main_outcomes
        accepted = publication(scope, output, "accepted", 11, check_assets)
        assert before[0]["wire.cc"] != accepted[0]["wire.cc"]
        assert before[0]["wire.h"] == accepted[0]["wire.h"]
        assert before[0]["app"] != accepted[0]["app"]
        edges = provenance(driver, scope, env, output, "accepted", definition, build, inspection)

        watches.replace_source(scope, "schema.txt", "rejected\n")
        summary, failed, _ = watch.build("one staged output then exit 7", succeeds=False)
        failure_status = summary["status"]
        assert failure_status == 7, summary
        build.check_wire_action(summary, failed, "failed")
        assert any("schema rejected" in diagnostic for diagnostic in summary["diagnostics"]), summary
        failure = [event for event in failed if event.get("kind") == "ExternalAction"
                   and event.get("key") == "wire" and event.get("outcome") == "failed"]
        assert len(failure) == 1, failure
        assert not build.action_keys(failed, "ProjectPublication"), failed
        assert all("executed" not in build.tu_outcomes(failed, name) for name in ("main.cc", "wire.cc"))
        build.check_program(app, 21)
        preserved = publication(scope, output, "after-exit-7", 11, check_assets)
        assert preserved == accepted, "failed action changed the executable or named publications"
        provenance(driver, scope, env, output, "after-exit-7", definition, build, inspection)

        app.unlink()
        watches.replace_source(scope, "schema.txt", "11\n")
        summary, restored, _ = watch.build("restore the last admitted receipt")
        assert summary["actions_executed"] == 0, summary
        assert not build.action_keys(restored, "ExternalAction"), restored
        build.check_program(app, 21)
        recovered = publication(scope, output, "restored", 11, check_assets)
        assert recovered[:2] == accepted[:2], "cached restoration changed published bytes"

        traces = {"initial": initial, "source_edit": updated, "exit_7": failed, "restored": restored}
        receipt_rows = {label: receipt_events(events) for label, events in traces.items()}
        receipt_keys = {label: {event["key"] for event in events} for label, events in receipt_rows.items()}
        matching = receipt_keys["source_edit"] & receipt_keys["restored"]
        assert matching, receipt_keys

        generated = {label: {name: generated_events(events, name) for name in ("wire.cc", "wire.h")}
                     for label, events in traces.items()}
        assert observed_size(generated["initial"]["wire.cc"], len(source_body(7)))
        assert observed_size(generated["initial"]["wire.h"], len(HEADER_BODY))
        assert observed_size(generated["source_edit"]["wire.cc"], len(source_body(11)))
        assert not observed_size(generated["exit_7"]["wire.cc"], len(PARTIAL_BODY))
        evidence = {
            "application_exit_statuses": [17, 21, 21, 21],
            "failed_tool_exit_status": failure_status,
            "failed_external_action": failure[0],
            "main_translation_unit_outcomes": sorted(main_outcomes),
            "publication_stamp_preserved": True,
            "restore_actions_executed": summary["actions_executed"],
            "independent_sha256": {"initial": before[0], "accepted": accepted[0],
                                   "after-exit-7": preserved[0], "restored": recovered[0]},
            "restored_receipt_trace_labels": sorted(matching),
            "action_receipt_events": receipt_rows,
            "generated_file_events": generated,
            "manifest_dependency_edges": edges,
        }
        (output / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
        return edges
    finally:
        watch.close()
        log.close()


def browser_evidence(output, edges):
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
            page.goto((output / "accepted-plan.html").as_uri(), wait_until="load")

            graph = page.locator("#target-graph")
            expect(graph).to_be_visible()
            details = graph.locator("#graph-details")
            for edge in edges:
                if edge["producer"] != "wire":
                    continue
                rows = details.locator(".graph-relation").filter(has_text=edge["usage"])
                row = rows.filter(has_text=f"Output {edge['output']}")
                expect(row).to_have_count(1)
                line = edge["location"]["line"]
                expect(row.locator(f"a[href='#manifest-line-{line}']")).to_have_count(1)
            graph.screenshot(path=output / "named-artifact-dependencies.png", animations="disabled")

            graph.get_by_role("button", name="wire", exact=True).press("Enter")
            facts = details.locator(".graph-fact")
            for name, filename in (("source", "wire.cc"), ("header", "wire.h")):
                row = facts.filter(has=page.get_by_text(f"{VIRTUAL}/{filename}", exact=True))
                expect(row).to_have_count(1)
                expect(row.locator(".fact-name")).to_have_text(name)
            graph.screenshot(path=output / "generator-outputs.png", animations="disabled")
            external = [url for url in requests if re.match(r"^(https?|wss?)://", url)]
            assert not errors and not external, (errors, external)
        finally:
            browser.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    parser.add_argument("repository", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--browser", action="store_true")
    args = parser.parse_args()
    driver, repo, output = [value.resolve() for value in (args.driver, args.repository, args.output)]
    output.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(repo / "tests/daemon"))
    import project_build as build
    import project_build_watch as watches
    import project_inspection as inspection
    from project_runtime_assets import check_assets
    from runtime_dir import runtime_root

    with (
        tempfile.TemporaryDirectory(prefix="ycxx-named-outputs-") as directory,
        tempfile.TemporaryDirectory(prefix="yo", dir=runtime_root("yo")) as runtime,
    ):
        scope = Path(directory).resolve()
        definition = prepare(scope, build)
        env = build.env_for(runtime)
        try:
            edges = watch_evidence(driver, scope, env, output, definition, build, watches, inspection, check_assets)
        finally:
            build.run_client(driver, scope, env, "ctl", "shutdown")
        if args.browser:
            browser_evidence(output, edges)
    print("App 17 → 21 → 21 → 21. Source changed, header and main TU reused. "
          "Exit 7 left both named publications and the app intact. Restore ran zero actions.")


if __name__ == "__main__":
    main()
