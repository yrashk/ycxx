"""Exercise the structured project module graph and legacy plan snapshots (#1363)."""

from collections import Counter
from copy import deepcopy
import json
from pathlib import Path
import sys
import tempfile

from project_build import daemon_identity, env_for, run_client, traced_build
from project_build_watch import replace_source
from project_module_providers import (
    CHOICE, CHOICES, CONFLICTS, EXPECTED_CYCLES, EXTERNAL, GENERATED, GENERATOR, SOURCES,
    build, check_plan, definition, displayed_paths, facts, prepare, reports, show,
)
from project_shader_bundle import check_external_actions
from project_targets import DARWIN, LINUX, executable, journal_contents, namespace
from runtime_dir import runtime_root


STATUS = {
    'resolved': 'resolved',
    'resolved in partial graph': 'provisional',
    'ambiguous': 'ambiguous',
    'no known provider in partial graph': 'pending',
    'unresolved partition': 'unresolved_partition',
    'external or unresolved': 'external_or_unresolved',
}


def integer(value, minimum):
    assert type(value) is int and value >= minimum, value
    return value


def site(location):
    path = location['path']
    assert type(path) is str and path, location
    line = integer(location['line'], 1)
    column = integer(location['column'], 1)
    return f'{path}:{line}:{column}'


def module_graph(plan, triple):
    assert 'module_graph' in plan, 'the resolved JSON plan did not include a structured module graph'
    graph = plan['module_graph']
    assert graph['native_target'] == triple and type(graph['complete']) is bool, graph
    return graph


def index_graph(graph):
    units = {}
    declarations = {}
    for unit in graph['units']:
        identifier = integer(unit['id'], 0)
        assert identifier not in units, unit
        units[identifier] = unit
        assert unit['scan'] in {'complete', 'preprocessing_incomplete', 'generated_inputs_unavailable'}, unit
        for declaration in unit['declarations']:
            key = identifier, integer(declaration['directive_index'], 0)
            assert key not in declarations, declaration
            declarations[key] = declaration
            assert declaration['kind'] in {'interface_unit', 'implementation_unit'}, declaration
            site(declaration['location'])
    assert graph['complete'] == all(unit['scan'] == 'complete' for unit in units.values()), graph

    for entry in graph['imports']:
        key = integer(entry['unit_id'], 0), integer(entry['directive_index'], 0)
        assert key[0] in units and key not in declarations, entry
        assert entry['kind'] in {'import', 'exported_import'}, entry
        site(entry['location'])
    for component in graph['cycles']:
        assert component and len(component) == len(set(component)) and set(component) <= units.keys(), component
    return units, declarations


def candidate_refs(entry, declarations):
    refs = [(integer(candidate['unit_id'], 0), integer(candidate['directive_index'], 0))
            for candidate in entry['candidates']]
    assert len(refs) == len(set(refs)), entry
    assert all(ref in declarations and declarations[ref]['name'] == entry['name'] for ref in refs), entry
    return refs


def check_graph(scope, plan, triple, *, incomplete=None, reason=None):
    graph = module_graph(plan, triple)
    old_providers, old_cycles = check_plan(scope, plan, triple, partial=incomplete is not None)
    units, declarations = index_graph(graph)

    expected_scans = dict.fromkeys(SOURCES, 'complete')
    if incomplete:
        expected_scans[incomplete] = reason
    assert {unit['source']: unit['scan'] for unit in units.values()} == expected_scans, units
    assert len(units) == len(expected_scans), units

    expected_declarations = Counter(
        ('interface_unit' if ' · interface unit ' in row['name'] else 'implementation_unit',
         row['value'], row['origin']) for row in facts(plan, 'module-unit')
    )
    assert Counter((entry['kind'], entry['name'], site(entry['location']))
                   for entry in declarations.values()) == expected_declarations, declarations

    previous_imports = {(row['value'], row['origin']): row for row in facts(plan, 'module-import')}
    previous_providers = {row['name']: row for row in old_providers}
    import_refs = set()
    for entry in graph['imports']:
        key = entry['unit_id'], entry['directive_index']
        assert key not in import_refs, entry
        import_refs.add(key)
        location = site(entry['location'])
        assert units[entry['unit_id']]['source'] == entry['location']['path'], entry
        old_import = previous_imports[entry['name'], location]
        kind = 'exported_import' if ' · export import ' in old_import['name'] else 'import'
        assert entry['kind'] == kind, entry
        provider = previous_providers[old_import['name']]
        assert entry['status'] == STATUS[provider['value'].split(' · ', 1)[0]], entry
        refs = candidate_refs(entry, declarations)
        from_legacy = provider['origin'].partition(' → ')[2]
        assert {site(declarations[ref]['location']) for ref in refs} == set(filter(None, from_legacy.split(', '))), entry
    assert len(import_refs) == len(previous_imports), import_refs

    conflicts = {entry['name']: entry for entry in graph['conflicts']}
    assert conflicts.keys() == CONFLICTS.keys() and len(conflicts) == len(graph['conflicts']), conflicts
    for old_conflict in facts(plan, 'module-provider-conflict'):
        refs = candidate_refs(conflicts[old_conflict['name']], declarations)
        paths = [units[identifier]['source'] for identifier, _ in refs]
        assert sorted(paths) == sorted(displayed_paths(old_conflict['value'].split(' · ', 1)[1])), old_conflict
    known_cycles = [frozenset(units[identifier]['source'] for identifier in cycle) for cycle in graph['cycles']]
    assert len(known_cycles) == len(old_cycles) and set(known_cycles) == EXPECTED_CYCLES, known_cycles

    choice, = (entry for entry in graph['imports'] if entry['name'] == CHOICE)
    provider, = candidate_refs(choice, declarations)
    assert units[provider[0]]['source'] == CHOICES[triple], choice
    assert declarations[provider]['location'] == {'path': CHOICES[triple], 'line': 3, 'column': 1}
    return graph


def check_provider_reuse(summary, events):
    assert summary['actions_executed'] == 0, summary
    semantic, _ = reports(events)
    assert {event['outcome'] for event in semantic} <= {'reused', 'verified', 'durable'}, semantic


def check_edits(ycxx, scope, env, journal, before):
    legacy = {key: value for key, value in before.items() if key != 'module_graph'}
    saved = scope.parent / 'old-schema-1.json'
    saved.write_text(json.dumps(legacy), encoding='utf-8')
    current_snapshot = scope.parent / 'with-module-graph.json'
    current_snapshot.write_text(json.dumps(before), encoding='utf-8')
    for snapshot in (saved, current_snapshot):
        diff = show(ycxx, scope, env, journal, LINUX, '--against', str(snapshot))
        assert diff == {'schema': 1, 'target': 'app', 'changes': []}, diff

    replace_source(scope, 'linux.cc', SOURCES['linux.cc'].replace('return 11;', 'return 12;'))
    summary, events = build(ycxx, scope, env, LINUX, 'body-only edit retains the structured graph')
    check_provider_reuse(summary, events)
    assert show(ycxx, scope, env, journal, LINUX) == before

    shifted = '\n\n' + SOURCES['main.cc']
    replace_source(scope, 'main.cc', shifted)
    moved = show(ycxx, scope, env, journal, LINUX)
    require_moved = deepcopy(before['module_graph'])
    for entry in require_moved['imports']:
        if entry['location']['path'] == 'main.cc':
            entry['location']['line'] += 2
    assert check_graph(scope, moved, LINUX) == require_moved, moved
    assert show(ycxx, scope, env, journal, LINUX, '--against', str(current_snapshot))['changes'] == []
    summary, events = build(ycxx, scope, env, LINUX, 'moving an import only updates its provenance')
    check_provider_reuse(summary, events)

    replace_source(scope, 'main.cc', shifted.replace(f'import {EXTERNAL};', '// removed the external import'))
    reduced = show(ycxx, scope, env, journal, LINUX)
    assert not any(entry['name'] == EXTERNAL for entry in module_graph(reduced, LINUX)['imports'])
    diff = show(ycxx, scope, env, journal, LINUX, '--against', str(saved))['changes']
    old_import, = (row for row in facts(before, 'module-import') if row['value'] == EXTERNAL)
    old_edge, = (row for row in facts(before, 'module-provider') if row['name'] == old_import['name'])
    assert len(diff) == 2 and all(row['change'] == 'removed' and row.get('after') is None for row in diff), diff
    assert [row['before'] for row in diff] == [old_import, old_edge], diff
    replace_source(scope, 'main.cc', shifted)


def check_project(ycxx, scope, env, journal):
    cold = show(ycxx, scope, env, journal, LINUX)
    assert facts(cold, 'unresolved') and 'module_graph' not in cold and not (scope / '.ycxx').exists(), cold

    build(ycxx, scope, env, LINUX, 'resolve the Linux project')
    before = show(ycxx, scope, env, journal, LINUX)
    check_graph(scope, before, LINUX)
    daemon = daemon_identity(ycxx, scope, env)
    assert daemon is not None
    build(ycxx, scope, env, DARWIN, 'resolve the Darwin project')
    check_graph(scope, show(ycxx, scope, env, journal, DARWIN), DARWIN)
    assert show(ycxx, scope, env, journal, LINUX) == before

    check_edits(ycxx, scope, env, journal, before)
    replace_source(scope, 'darwin.cc', '#include "missing-fixture.h"\n' + SOURCES['darwin.cc'])
    partial = show(ycxx, scope, env, journal, LINUX)
    check_graph(scope, partial, LINUX, incomplete='darwin.cc', reason='preprocessing_incomplete')
    replace_source(scope, 'darwin.cc', SOURCES['darwin.cc'])

    replace_source(scope, 'build.ycxx', definition(journal, generated=True))
    _, events = build(ycxx, scope, env, LINUX, 'admit a generated module source')
    check_external_actions(events, GENERATOR)
    generated = str(namespace(LINUX) / 'virtual' / GENERATOR / GENERATED)
    partial = show(ycxx, scope, env, journal, LINUX)
    check_graph(scope, partial, LINUX, incomplete=generated, reason='generated_inputs_unavailable')
    assert journal_contents(journal) == 'generated\n' and daemon_identity(ycxx, scope, env) == daemon


def check_configurations(ycxx, scope, env, journal):
    scope.mkdir()
    (scope / 'main.cc').write_text('int main() { return 0; }\n', encoding='utf-8')
    # Discovery precedes semantic checks. The preceding import makes the first declaration's
    # directive index differ from its offset in the declarations array.
    (scope / 'shared.cc').write_text('''\
module;
#include <config.hh>
#if SIDE == 1
import external.first;
export module cycle.left;
import cycle.left;
#else
export module cycle.right;
import cycle.right;
#endif
''', encoding='utf-8')
    for directory, side in (('left-include', 1), ('right-include', 2)):
        (scope / directory).mkdir()
        (scope / directory / 'config.hh').write_text(f'#define SIDE {side}\n', encoding='utf-8')
    (scope / 'build.ycxx').write_text('''\
cpp.library "left" { sources = ["shared.cc"] public_include_dirs = ["left-include"] }
cpp.library "right" { sources = ["shared.cc"] public_include_dirs = ["right-include"] }
cpp.library "again" { sources = ["shared.cc"] public_include_dirs = ["left-include"] }
cpp.executable "app" { sources = ["main.cc"] deps = ["left", "right", "again"] }
''', encoding='utf-8')
    build(ycxx, scope, env, LINUX, 'resolve repeated and distinct configured units')
    plan = show(ycxx, scope, env, journal, LINUX)
    graph = module_graph(plan, LINUX)
    units, declarations = index_graph(graph)
    shared = [unit for unit in units.values() if unit['source'] == 'shared.cc']
    assert len(shared) == 2 and len(units) == 3 and graph['complete'], units
    assert len(facts(plan, 'module-import')) == 5 and len(graph['imports']) == 3, graph
    assert len(graph['cycles']) == 2 and {tuple(cycle) for cycle in graph['cycles']} == {(unit['id'],) for unit in shared}

    self_imports = [entry for entry in graph['imports'] if entry['name'].startswith('cycle.')]
    assert {entry['name'] for entry in self_imports} == {'cycle.left', 'cycle.right'}, self_imports
    for edge in self_imports:
        reference, = candidate_refs(edge, declarations)
        unit = units[edge['unit_id']]
        expected_index = int(edge['name'] == 'cycle.left')
        assert edge['status'] == 'resolved' and reference == (unit['id'], expected_index), edge
        declaration, = unit['declarations']
        assert declaration['directive_index'] == expected_index and edge['directive_index'] == expected_index + 1, edge


def check_empty_graph(ycxx, scope, env, journal):
    replace_source(scope, 'build.ycxx', 'cpp.executable "app" { sources = ["main.cc"] }\n')
    traced_build(ycxx, scope, env, 'resolve a host-default project without modules')
    (scope / executable(None)).unlink()
    host = {'linux': LINUX, 'darwin': DARWIN}[sys.platform]
    graph = module_graph(show(ycxx, scope, env, journal, None), host)
    units, declarations = index_graph(graph)
    unit, = units.values()
    assert unit['source'] == 'main.cc' and graph['complete'] and not declarations, graph
    assert graph['imports'] == graph['conflicts'] == graph['cycles'] == [], graph


def main():
    ycxx = Path(sys.argv[1]).resolve()
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-structured-module-graph-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        scope = root / 'project'
        journal = root / 'runs'
        env = env_for(runtime)
        prepare(scope, journal)
        try:
            check_project(ycxx, scope, env, journal)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')

        scope = root / 'configurations'
        journal = root / 'unused-journal'
        try:
            check_configurations(ycxx, scope, env, journal)
            check_empty_graph(ycxx, scope, env, journal)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
