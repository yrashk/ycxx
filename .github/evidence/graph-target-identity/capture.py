#!/usr/bin/env python3
"""Verify and capture the actual #1611 daemon fixture reports with offline Chromium.

The input directory contains identity.html and identity.json from
identity_report(...), and partial.html and partial.json from
partial_component_report(...). Write each returned document verbatim and
serialize its returned plan as JSON. The capture does not run ycxx.

Example:
    python3 capture.py --reports /tmp/ycxx-reports --out /tmp/ycxx-evidence
"""

import argparse
import json
from pathlib import Path
import re
import shutil
from urllib.parse import urlsplit

from playwright.sync_api import expect, sync_playwright


CARD = 'section[aria-labelledby="target-dependencies"] > article.plan-target'
NODE = 'button.graph-node[data-graph-target]'
LOCAL_SCHEMES = {"about", "blob", "data", "file"}
SCANNERS = {
    "raw": ("scanner\x7f", "scanner_raw.cc"),
    "literal": (r"scanner\x7F", "scanner_literal.cc"),
}


def card_named(page, name):
    heading = page.get_by_role("heading", name=name, exact=True, level=3)
    return page.locator(CARD).filter(has=heading)


def node_selector(target_id):
    assert re.fullmatch(r"plan-target-[0-9]+", target_id), target_id
    return f'{NODE}[data-graph-target="{target_id}"]'


def visible_ids(page):
    ids = page.locator(f"{NODE}:visible").evaluate_all(
        "(nodes) => nodes.map(node => node.dataset.graphTarget)")
    assert len(ids) == len(set(ids)), ids
    return set(ids)


def selected_id(page):
    selected = page.locator(f'{NODE}[aria-pressed="true"]:visible')
    expect(selected).to_have_count(1)
    return selected.get_attribute("data-graph-target")


def key_select(page, target_id):
    # Start at the focusable graph viewport and let real Tab presses move to
    # the browser's focusable nodes. Never force focus on the selected button.
    selector = node_selector(target_id)
    node = page.locator(selector)
    expect(node).to_be_visible()
    page.locator("#graph-viewport").focus()
    for tabs in range(1, len(visible_ids(page)) + 1):
        page.keyboard.press("Tab")
        if page.evaluate("(selector) => document.activeElement.matches(selector)", selector):
            page.keyboard.press("Enter")
            expect(node).to_have_attribute("aria-pressed", "true")
            assert selected_id(page) == target_id
            return tabs
    raise AssertionError(f"Graph node {target_id} was not reachable with Tab")


def coords(location):
    path, line, column = (location[name] for name in ("path", "line", "column"))
    return f"{path}:{line}:{column}", f"#manifest-line-{line}"


def relation_in(page, scope, edge):
    coordinate, fragment = coords(edge["location"])
    rows = scope.filter(has=page.get_by_text(coordinate, exact=True))
    expect(rows).to_have_count(1)
    row = rows.first
    expect(row.locator("code.origin")).to_have_text(edge["usage"])
    for field, label in (("binding", "Binding"), ("output", "Output")):
        if edge.get(field) is not None:
            expect(row.get_by_text(f"{label} {edge[field]}", exact=True)).to_be_visible()
    where = row.locator(f'a[href="{fragment}"]')
    expect(where).to_have_text(coordinate)
    return row, where


def click_location(page, link):
    fragment = link.get_attribute("href")
    assert re.fullmatch(r"#manifest-line-[0-9]+", fragment), fragment
    link.click()
    assert page.locator(fragment).evaluate("(line) => line.matches(':target')"), page.url


def fact_expected(fact, label):
    coordinate, href = coords({"path": "build.ycxx", "line": fact["line"], "column": fact["column"]})
    return {"label": label, "value": fact["value"], "origin": fact["origin"], "site": coordinate, "href": href}


def identity(page, plan, out):
    assert plan["target"] == "app" and plan["dependency_graph"]["complete"]
    app = card_named(page, "app").get_attribute("id")
    pair = card_named(page, "pair")
    pair_id = pair.get_attribute("id")
    assert visible_ids(page) == {app, pair_id}
    assert selected_id(page) == app
    key_select(page, pair_id)
    page.locator("#graph-expand-inputs").click()

    expected = {}
    for binding, (name, source) in SCANNERS.items():
        edge, = [edge for edge in plan["dependency_graph"]["edges"]
                 if edge["consumer"] == "pair" and edge["producer"] == name and edge.get("binding") == binding]
        row, _ = relation_in(page, pair.locator(":scope > ul > li"), edge)
        target_link = row.locator('a[href^="#plan-target-"]')
        target_id = target_link.get_attribute("href")[1:]
        expect(page.locator(f'#{target_id} > h3 > code')).to_have_text(r"scanner\x7F")

        facts = [fact for fact in plan["facts"] if fact["target"] == name]
        source_fact, = [fact for fact in facts if fact["kind"] == "source"]
        output_fact, = [fact for fact in facts if fact["kind"] == "output"]
        assert Path(source_fact["value"]).name == source, source_fact
        assert output_fact["name"] == "executable", output_fact
        expected[binding] = (target_id, [fact_expected(source_fact, "Source"),
                                        fact_expected(output_fact, "Output")])

    scanner_ids = {target_id for target_id, _ in expected.values()}
    assert len(scanner_ids) == 2
    assert visible_ids(page) == {app, pair_id, *scanner_ids}

    evidence = {}
    for binding, (target_id, facts) in expected.items():
        node = page.locator(node_selector(target_id))
        expect(node).to_have_text(r"scanner\x7F")
        tabs = key_select(page, target_id)
        actual = page.locator("#graph-details li.graph-fact").evaluate_all(
            """rows => rows.map(row => ({
                label: row.querySelector('strong')?.textContent,
                value: row.querySelector('.fact-value')?.textContent,
                origin: row.querySelector('.origin')?.textContent,
                site: row.querySelector('.location')?.textContent,
                href: row.querySelector('.location a')?.getAttribute('href')
            })).filter(row => row.label === 'Source' || row.label === 'Output')""")
        assert sorted(actual, key=lambda fact: fact["label"]) == sorted(facts, key=lambda fact: fact["label"]), (
            binding, actual, facts)
        for fact in facts:
            kind = fact["label"].lower()
            expect(page.locator(f'tr[data-graph-target="{target_id}"][data-graph-kind="{kind}"]')).to_have_count(1)
            expect(page.locator(fact["href"])).to_have_count(1)
        evidence[binding] = {"id": target_id, "tabs": tabs, "facts": actual}
        page.screenshot(path=out / f"identity-{binding}.png")
    return evidence


def partial_edge(plan):
    graph = plan["dependency_graph"]
    assert plan["target"] == "app" and not graph["complete"]
    assert all(edge["consumer"] != "app" for edge in graph["edges"])
    edge, = [edge for edge in graph["edges"] if edge["consumer"] == "retained" and edge["producer"] == "resolved"]
    assert (edge["usage"], edge["binding"], edge["output"]) == ("inputs", "first", "text")
    return edge


def partial(page, plan, out):
    edge = partial_edge(plan)
    app = card_named(page, "app").get_attribute("id")
    retained = card_named(page, "retained").get_attribute("id")
    resolved = card_named(page, "resolved").get_attribute("id")
    starting = visible_ids(page)
    assert starting == {app, retained, resolved}, starting
    assert selected_id(page) == app
    expect(page.locator("#target-graph .dependency-notice")).to_contain_text("Incomplete plan")
    # An isolated root plus the retained component has just one visible
    # connection. Check the actual canvas received a painted stroke.
    page.wait_for_function("""() => {
        const canvas = document.querySelector('#graph-edges');
        if (!canvas?.width || !canvas.height) return false;
        const pixels = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
        for (let i = 3; i < pixels.length; i += 4) if (pixels[i]) return true;
        return false;
    }""")
    tabs = key_select(page, retained)
    row, where = relation_in(page, page.locator("#graph-details li.graph-relation"), edge)
    expect(row.locator('a[href^="#plan-target-"]')).to_have_attribute("href", "#" + resolved)
    click_location(page, where)
    page.locator("#target-graph").scroll_into_view_if_needed()
    page.screenshot(path=out / "partial-component.png")

    key_select(page, resolved)
    page.locator("#graph-reset").click()
    assert selected_id(page) == app
    assert visible_ids(page) == starting
    return {"targets": sorted(starting), "tabs": tabs, "edge": edge, "reset": app}


def static_partial(page, plan, out):
    expect(page.locator("#target-graph")).to_be_hidden()
    edge = partial_edge(plan)
    retained = card_named(page, edge["consumer"])
    resolved = card_named(page, edge["producer"])
    expect(retained).to_be_visible()
    expect(resolved).to_be_visible()
    row, where = relation_in(page, retained.locator(":scope > ul > li"), edge)
    link = row.locator('a[href^="#plan-target-"]')
    expect(link).to_have_attribute("href", "#" + resolved.get_attribute("id"))
    link.click()
    assert resolved.evaluate("(card) => card.matches(':target')")
    click_location(page, where)
    page.locator('section[aria-labelledby="target-dependencies"]').scroll_into_view_if_needed()
    page.screenshot(path=out / "partial-static.png")


def capture(browser, report, plan, out, check, *, javascript=True):
    attempted = []
    errors = []
    context = browser.new_context(java_script_enabled=javascript, service_workers="block",
                                  viewport={"width": 1460, "height": 920}, device_scale_factor=1)
    try:
        context.route(re.compile(r"^(?:https?|wss?|ftp):"), lambda route: route.abort())
        context.on("request", lambda request: attempted.append(request.url)
                   if urlsplit(request.url).scheme not in LOCAL_SCHEMES else None)
        page = context.new_page()
        page.on("websocket", lambda socket: attempted.append(socket.url))
        page.on("pageerror", lambda error: errors.append(str(error)))
        page.goto(report.as_uri(), wait_until="load")
        if javascript:
            expect(page.locator("#target-graph")).to_be_visible()
        proof = check(page, plan, out)
    finally:
        context.close()
    assert not attempted, attempted
    assert not errors, errors
    return {"network_requests": len(attempted), "browser_errors": errors, "proof": proof}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reports", type=Path, required=True, help="directory with the four emitted fixture files")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--chromium", type=Path)
    options = parser.parse_args()
    out = options.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for name in ("identity", "partial"):
        for suffix in (".html", ".json"):
            original = (options.reports / (name + suffix)).resolve(strict=True)
            saved = out / (name + suffix)
            if original != saved:
                shutil.copyfile(original, saved)
    chromium = options.chromium or shutil.which("chromium") or shutil.which("chromium-browser")
    assert chromium, "Specify Chromium with --chromium"
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(executable_path=str(chromium),
                                             args=["--no-sandbox", "--disable-dev-shm-usage",
                                                   "--disable-background-networking"])
        cases = [("identity", "identity", identity, True),
                 ("partial", "partial", partial, True),
                 ("partial_static", "partial", static_partial, False)]
        evidence = {}
        for name, document, check, javascript in cases:
            plan = json.loads((out / (document + ".json")).read_text(encoding="utf-8"))
            evidence[name] = capture(browser, out / (document + ".html"), plan, out,
                                     check, javascript=javascript)
        browser.close()
    (out / "verification.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
