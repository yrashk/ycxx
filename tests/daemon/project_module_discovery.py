"""Inspect and reuse project module dependency discovery (#1349)."""

import html
import json
from pathlib import Path
import sys
import tempfile

from project_build import daemon_identity, env_for, run_client, traced_build
from project_build_watch import replace_source
from project_inspection import fact
from project_shader_bundle import check_external_actions
from project_targets import DARWIN, LINUX, executable, journal_contents, namespace, target_args
from runtime_dir import runtime_root


IMPORTER = 'main.cc'
INTERFACE = 'surface.cc'
GUARD = 'guarded.cc'
CONFIG = 'config/module_options.h'
GENERATED = 'generated.cc'
UNITS = {LINUX: 'selected.linux', DARWIN: 'selected.darwin'}
KINDS = {'module-unit', 'module-import'}
NOISE = ('commented.out', 'quoted.out', 'raw.out', 'dead.attribute', 'missing.only_during_show')
IMPORTER_TEXT = '''\
#include <module_options.h>
import demo.surface;
#if ENABLE_SELECTED_MODULE
#if defined(__x86_64__)
import selected.linux;
#elif defined(__aarch64__)
import selected.darwin;
#endif
#endif

import dead.attribute [[bad(]];
// import commented.out;
const char* quoted = "import quoted.out;";
const char* raw = R"noise(
export module raw.out;
import raw.out;
)noise";
int module = 1;
int import = 2;
int main() { return module + import; }
'''
INTERFACE_TEXT = '''\
module;
#line 200 "logical-module.cc"
export module demo.surface;
export int surface_value() { return 7; }
module :private;
'''
GUARDED_TEXT = '''\
#if !__has_include("guard.h")
import missing.only_during_show;
#endif
int guarded_value() { return 0; }
'''
GENERATED_TEXT = '''\
#if defined(__x86_64__)
export module selected.linux;
#elif defined(__aarch64__)
export module selected.darwin;
#endif
export int selected_value() { return 3; }
'''
GENERATE = r'''
printf 'generated\n' >> "$1" || exit 1
/bin/cat "$2" > "$3" || exit 1
printf '#define MODULE_GUARD 1\n' > "$4"
'''


def prepare(scope, journal):
    (scope / 'config').mkdir(parents=True)
    (scope / CONFIG).write_text('#define ENABLE_SELECTED_MODULE 1\n', encoding='utf-8')
    (scope / IMPORTER).write_text(IMPORTER_TEXT, encoding='utf-8')
    (scope / INTERFACE).write_text(INTERFACE_TEXT, encoding='utf-8')
    (scope / GUARD).write_text(GUARDED_TEXT, encoding='utf-8')
    (scope / 'provider.in').write_text(GENERATED_TEXT, encoding='utf-8')
    (scope / 'build.ycxx').write_text(f'''\
generate "provider" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(GENERATE)}, "provider", {json.dumps(str(journal))}, input("template"),
            output("source"), output("header")]
    inputs = {{ template = "provider.in" }}
    outputs = {{ source = "{GENERATED}", header = "guard.h" }}
}}
cpp.library "surface" {{
    sources = ["{INTERFACE}"]
    public_include_dirs = ["config"]
}}
cpp.library "virtual_guard" {{
    sources = ["{GUARD}"]
    generated_headers = [artifact("provider", "header")]
}}
cpp.executable "app" {{
    sources = ["{IMPORTER}", artifact("provider", "source")]
    deps = ["surface", "virtual_guard"]
}}
''', encoding='utf-8')


def show(ycxx, scope, env, journal, triple, *args, format_='json'):
    before = journal_contents(journal)
    result = run_client(ycxx, scope, env, 'show', 'app', *target_args(triple), f'--format={format_}', *args)
    assert result.returncode == 0, (triple, result.stderr)
    assert journal_contents(journal) == before, 'show ran the source generator'
    assert not (scope / executable(triple)).exists(), 'show published an executable'
    return json.loads(result.stdout) if format_ == 'json' else result.stdout


def source_location(row, path, text, directive):
    offset = text.index(directive)
    line = text.count('\n', 0, offset) + 1
    column = offset - text.rfind('\n', 0, offset)
    assert row['origin'] == f'{path}:{line}:{column}', row
    assert row['line'] == row['column'] == 0, row
    assert path in row['name'], row


def check_plan(plan, triple, importer=IMPORTER_TEXT, *, conditional=True):
    assert plan['schema'] == 1 and plan['target'] == 'app', plan
    rows = [row for row in plan['facts'] if row['kind'] in KINDS]
    expected = {('module-unit', 'demo.surface'), ('module-import', 'demo.surface')}
    if conditional:
        expected.add(('module-import', UNITS[triple]))
    assert {(row['kind'], row['value']) for row in rows} == expected and len(rows) == len(expected), rows
    assert all(row['target'] == 'app' for row in rows), rows
    names = {(row['kind'], row['value']): row for row in rows}
    assert 'interface unit' in names['module-unit', 'demo.surface']['name'], names
    source_location(names['module-unit', 'demo.surface'], INTERFACE, INTERFACE_TEXT, 'export module demo.surface;')
    for kind, name in expected:
        if kind == 'module-import':
            source_location(names[kind, name], IMPORTER, importer, f'import {name};')
    generated_path = str(namespace(triple) / 'virtual/provider' / GENERATED)
    pending = [row for row in plan['facts'] if row['kind'] == 'module-scan-pending']
    assert len(pending) == 2 and {row['name'] for row in pending} == {json.dumps(generated_path), json.dumps(GUARD)}, pending
    include = fact(plan, 'include', 'app', 'config')
    assert 'surface' in include['origin'] and 'public_include_dirs' in include['origin'], include
    return names


def trace_events(events, kind, source=None):
    return [event for event in events if event.get('kind') == kind
            and (source is None or Path(event.get('span', {}).get('file', event['key'])).name == source)]


def build(ycxx, scope, env, triple, label):
    summary, events = traced_build(ycxx, scope, env, label, *target_args(triple), succeeds=False)
    assert any('module' in diagnostic.lower() for diagnostic in summary['diagnostics']), summary
    assert not (scope / executable(triple)).exists(), 'failed module semantics published an executable'
    graph = trace_events(events, 'ModuleGraph')
    assert graph, 'build did not trace the module graph'
    return summary, events, graph[-1]


def check_live_graph(graph, events, triple, *, conditional=True):
    value = graph['value']
    assert all(part in value for part in (IMPORTER, INTERFACE, GENERATED, 'demo.surface', UNITS[triple])), graph
    assert UNITS[DARWIN if triple == LINUX else LINUX] not in value, graph
    assert all(part not in value for part in (*NOISE, 'logical-module.cc')), graph

    generated = trace_events(events, 'ModuleDirectives', GENERATED)
    declaration = f'export module {UNITS[triple]};'
    line = GENERATED_TEXT.count('\n', 0, GENERATED_TEXT.index(declaration)) + 1
    assert generated and f'{GENERATED}:{line}:1: interface unit {UNITS[triple]}' in generated[-1]['value'], generated
    importer = trace_events(events, 'ModuleDirectives', IMPORTER)
    assert importer and 'demo.surface' in importer[-1].get('value', ''), importer
    assert (UNITS[triple] in importer[-1]['value']) == conditional, importer
    guarded = trace_events(events, 'ModuleDirectives', GUARD)
    assert guarded and guarded[-1].get('value') == 'complete', guarded


def check_edits(ycxx, scope, env, journal, before, original_graph):
    snapshot = scope.parent / 'before.json'
    snapshot.write_text(json.dumps(before), encoding='utf-8')
    replace_source(scope, INTERFACE, INTERFACE_TEXT.replace('return 7;', 'return 8;'))
    summary, events, graph = build(ycxx, scope, env, LINUX, 'module body edit keeps the dependency graph')
    assert summary['actions_executed'] == 0, summary
    assert any(event['outcome'] == 'executed' for event in trace_events(events, 'Pp', INTERFACE)), events
    assert any(event['outcome'] == 'cutoff' for event in trace_events(events, 'ModuleDirectives', INTERFACE)), events
    assert {event['outcome'] for event in trace_events(events, 'ModuleGraph')} <= {'reused', 'verified', 'durable'}, events
    assert graph['value'] == original_graph['value'], graph

    shifted = '\n\n' + IMPORTER_TEXT
    replace_source(scope, IMPORTER, shifted)
    moved = show(ycxx, scope, env, journal, LINUX)
    check_plan(moved, LINUX, shifted)
    assert show(ycxx, scope, env, journal, LINUX, '--against', str(snapshot))['changes'] == [], moved
    summary, events, graph = build(ycxx, scope, env, LINUX, 'module import location changes without an edge change')
    assert summary['actions_executed'] == 0, summary
    assert any(event['outcome'] == 'executed' for event in trace_events(events, 'ModuleDirectives', IMPORTER)), events
    assert any(event['outcome'] == 'cutoff' for event in trace_events(events, 'ModuleGraph')), events
    assert graph['value'] == original_graph['value'], graph

    replace_source(scope, CONFIG, '#define ENABLE_SELECTED_MODULE 0\n')
    after = show(ycxx, scope, env, journal, LINUX)
    check_plan(after, LINUX, shifted, conditional=False)
    changes = show(ycxx, scope, env, journal, LINUX, '--against', str(snapshot))['changes']
    old_import = check_plan(before, LINUX)['module-import', UNITS[LINUX]]
    old_provider = next(row for row in before['facts']
                        if row['kind'] == 'module-provider' and row['name'] == old_import['name'])
    assert len(changes) == 2, changes
    assert all(change['change'] == 'removed' and change.get('after') is None for change in changes), changes
    assert {change['before']['kind']: change['before'] for change in changes} == {
        'module-import': old_import, 'module-provider': old_provider
    }, changes

    summary, events, graph = build(ycxx, scope, env, LINUX, 'included manifest configuration drops an import')
    assert summary['actions_executed'] == 0, summary
    assert any(event['outcome'] == 'executed' for event in trace_events(events, 'ModuleGraph')), events
    assert graph['value'] != original_graph['value'], graph
    check_live_graph(graph, events, LINUX, conditional=False)


def check_builds(ycxx, scope, env, journal):
    for triple in (LINUX, DARWIN):
        cold = show(ycxx, scope, env, journal, triple)
        assert not any(row['kind'] in KINDS for row in cold['facts']), cold
        assert not (scope / '.ycxx').exists(), 'cold show created build state'

    _, events, linux = build(ycxx, scope, env, LINUX, 'discover Linux module declarations and imports')
    check_external_actions(events, 'provider')
    assert any(event['outcome'] == 'executed' for event in trace_events(events, 'ModuleGraph')), events
    check_live_graph(linux, events, LINUX)
    before = show(ycxx, scope, env, journal, LINUX)
    check_plan(before, LINUX)
    daemon = daemon_identity(ycxx, scope, env)
    assert daemon is not None and daemon[0] == str(scope), daemon

    _, events, darwin = build(ycxx, scope, env, DARWIN, 'discover Darwin module declarations and imports')
    check_external_actions(events)
    assert any(event['outcome'] == 'executed' for event in trace_events(events, 'ModuleGraph')), events
    check_live_graph(darwin, events, DARWIN)
    darwin_plan = check_plan(show(ycxx, scope, env, journal, DARWIN), DARWIN)
    check_plan(show(ycxx, scope, env, journal, LINUX), LINUX)

    document = html.unescape(show(ycxx, scope, env, journal, DARWIN, format_='html'))
    conditional_import = darwin_plan['module-import', UNITS[DARWIN]]
    for term in ('demo.surface', UNITS[DARWIN], conditional_import['origin'], GENERATED):
        assert term in document, (term, document)
    assert all(term not in document for term in (*NOISE, UNITS[LINUX])), document

    check_edits(ycxx, scope, env, journal, before, linux)
    assert journal_contents(journal) == 'generated\n', 'inspection or target alternation reran the generator'
    assert daemon_identity(ycxx, scope, env) == daemon


def main():
    ycxx = Path(sys.argv[1]).resolve()
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-module-discovery-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        scope = root / 'project'
        journal = root / 'module-runs'
        env = env_for(runtime)
        prepare(scope, journal)
        try:
            check_builds(ycxx, scope, env, journal)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
