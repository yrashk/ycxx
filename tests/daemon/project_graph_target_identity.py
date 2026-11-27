"""Preserve target identities and resolved components in HTML graphs (#1611)."""

import json
from pathlib import Path
import sys
import tempfile

from project_build import env_for, run_client, traced_build
from project_build_watch import replace_source
from project_inspection import (
    check_dependency_graph, check_dependency_html, check_location, check_target_dependencies_html,
    check_target_graph_html, dependency_edge, fact, graph_target_ids, scope_state,
)
from project_module_graph import PlanHTML
from project_shader_bundle import check_external_actions
from runtime_dir import runtime_root


RAW = 'scanner\x7f'
LITERAL = r'scanner\x7F'
SCANNERS = ((RAW, 'scanner_raw.cc', 11), (LITERAL, 'scanner_literal.cc', 22))
PAIR = r'''
"$1"
[ "$?" -eq 11 ] || exit 7
"$2"
[ "$?" -eq 22 ] || exit 8
printf '#pragma once\n' > "$3"
'''
JOIN = r'/bin/cat "$1" "$2" > "$3"'


def spelling(name):
    # The manifest gets a raw DEL in one string and an escaped literal backslash in the other.
    return json.dumps(name).replace(r'\u007f', '\x7f')


def show(ycxx, scope, env):
    before = scope_state(scope)
    raw = run_client(ycxx, scope, env, 'show', 'app', '--format=json')
    html = run_client(ycxx, scope, env, 'show', 'app', '--format=html')
    assert raw.returncode == html.returncode == 0, (raw.stderr, html.stderr)
    assert scope_state(scope) == before, 'show changed the workspace'
    return json.loads(raw.stdout), html.stdout


def identity_report(ycxx, scope, env):
    scope.mkdir()
    for _, source, code in SCANNERS:
        (scope / source).write_text(f'int main() {{ return {code}; }}\n', encoding='utf-8')
    (scope / 'main.cc').write_text('#include "pair.hh"\nint main() { return 0; }\n', encoding='utf-8')
    definition = '\n'.join(f'cpp.host_executable {spelling(name)} {{ sources = [{json.dumps(source)}] }}'
                           for name, source, _ in SCANNERS)
    raw = f'artifact({spelling(RAW)}, "executable")'
    literal = f'artifact({spelling(LITERAL)}, "executable")'
    definition += f'''

generate "pair" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(PAIR)}, "pair", input("raw"), input("literal"), output("header")]
    inputs = {{ raw = {raw}
                literal = {literal} }}
    outputs = {{ header = "pair.hh" }}
}}
cpp.executable "app" {{
    sources = ["main.cc"]
    generated_headers = [artifact("pair", "header")]
}}
'''
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    _, events = traced_build(ycxx, scope, env, 'admit both same-display host scanner targets')
    check_external_actions(events, 'pair')
    plan, document = show(ycxx, scope, env)
    check_dependency_graph(plan, [
        dependency_edge(definition, 'app', 'pair', 'generated_headers', 'artifact("pair", "header")', output='header'),
        dependency_edge(definition, 'pair', RAW, 'inputs', raw, output='executable', binding='raw'),
        dependency_edge(definition, 'pair', LITERAL, 'inputs', literal, output='executable', binding='literal'),
    ])
    for name, source, _ in SCANNERS:
        check_location(fact(plan, 'source', name, source), definition, json.dumps(source))
        output = fact(plan, 'output', name, 'executable')
        assert output['origin'] == 'linked host executable', output
        check_location(output, definition, f'cpp.host_executable {spelling(name)}')
    return plan, document


def partial_component_report(ycxx, scope, env):
    scope.mkdir()
    (scope / 'main.cc').write_text('#include "joined.hh"\nint main() { return 0; }\n', encoding='utf-8')
    for filename, contents in (('first.txt', '// first\n'), ('before.txt', '// second\n'), ('after.txt', '// updated\n')):
        (scope / filename).write_text(contents, encoding='utf-8')
    first = 'artifact("resolved", "text")'
    definition = f'''\
generate "resolved" {{
    tool = "/bin/cp"
    args = [input("file"), output("text")]
    inputs = {{ file = "first.txt" }}
    outputs = {{ text = "first.hh" }}
}}
generate "late" {{
    tool = "/bin/cp"
    args = [input("file"), output("text")]
    inputs = {{ file = "before.txt" }}
    outputs = {{ text = "second.hh" }}
}}
generate "retained" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(JOIN)}, "join", input("first"), input("second"), output("header")]
    inputs = {{ first = {first}, second = artifact("late", "text") }}
    outputs = {{ header = "joined.hh" }}
}}
cpp.executable "app" {{
    sources = ["main.cc"]
    generated_headers = [artifact("retained", "header")]
}}
'''
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    _, events = traced_build(ycxx, scope, env, 'admit both inputs of the generated header')
    check_external_actions(events, 'resolved', 'late', 'retained')
    definition = definition.replace('"before.txt"', '"after.txt"')
    replace_source(scope, 'build.ycxx', definition)
    plan, document = show(ycxx, scope, env)
    check_dependency_graph(plan, [
        dependency_edge(definition, 'retained', 'resolved', 'inputs', first, output='text', binding='first'),
    ], complete=False)
    assert fact(plan, 'unresolved', 'late', 'standard/rules')['name'] == 'generate'
    return plan, document


def check_identity(plan, document):
    html = PlanHTML(document)
    cards = html.section('target-dependencies', 'Target dependencies')
    ids = graph_target_ids(plan)
    targets = {node.attrs['id']: node for node in cards.descendants if node.tag == 'article'}
    assert ids[RAW] != ids[LITERAL]
    for name, _, _ in SCANNERS:
        card = targets[ids[name]]
        title, = (node for node in card.descendants if node.tag == 'h3')
        assert title.visible().strip() == LITERAL, title.visible()
    check_target_graph_html(document, plan)


def check_partial(plan, document):
    check_dependency_html(document, plan)
    check_target_dependencies_html(document, plan)
    check_target_graph_html(document, plan)
    graph = PlanHTML(document).section('target-graph-heading', 'Graph')
    assert 'data-partial' in graph.attrs, graph.attrs


def main():
    ycxx = Path(sys.argv[1]).resolve()
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-graph-target-identity-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        env = env_for(runtime)
        for name, prepare, check in (
            ('identity', identity_report, check_identity),
            ('partial', partial_component_report, check_partial),
        ):
            scope = root / name
            try:
                check(*prepare(ycxx, scope, env))
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
