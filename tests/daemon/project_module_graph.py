"""Check project module graphs and comparisons in JSON and HTML (#1363, #1378, #1383, #1390)."""

from collections import Counter
from copy import deepcopy
from html.parser import HTMLParser
import json
from pathlib import Path
import re
import sys
import tempfile

from project_build import daemon_identity, env_for, run_client, traced_build
from project_build_watch import replace_source
from project_module_providers import (
    CHOICE, CHOICES, CONFLICTS, EXPECTED_CYCLES, EXTERNAL, GENERATED, GENERATOR, SOURCES,
    build, check_plan, definition, displayed_paths, facts, prepare, reports, show, state,
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
UNSAFE_SOURCE = 'x<&>.cc'
UNSAFE_SNAPSHOT_SITE = '<script data-module-probe="1">&\'saved\'</script>'


class HTMLElement:
    def __init__(self, tag, attributes, parent):
        self.tag = tag
        self.attrs = dict(attributes)
        self.parent = parent
        self.descendants = []
        self.text = []

    def visible(self):
        return ' '.join(self.text)


class PlanHTML(HTMLParser):
    VOID = {'area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'param',
            'source', 'track', 'wbr'}

    def __init__(self, document):
        super().__init__()
        self.document = document
        self.elements = []
        self.open = []
        self.feed(document)
        self.close()

    def handle_starttag(self, tag, attrs):
        node = HTMLElement(tag, attrs, self.open[-1] if self.open else None)
        self.elements.append(node)
        for parent in self.open:
            parent.descendants.append(node)
        if tag not in self.VOID:
            self.open.append(node)

    def handle_endtag(self, tag):
        for index in range(len(self.open) - 1, -1, -1):
            if self.open[index].tag == tag:
                del self.open[index:]
                break

    def handle_data(self, data):
        for node in self.open:
            node.text.append(data)

    def sections(self, identifier):
        return [node for node in self.elements
                if node.tag == 'section' and node.attrs.get('aria-labelledby') == identifier]

    def section(self, identifier, title):
        sections = self.sections(identifier)
        assert len(sections) == 1, ('expected one HTML section', identifier, len(sections))
        section, = sections
        headings = [node.visible().strip() for node in section.descendants
                    if node.tag == 'h2' and node.attrs.get('id') == identifier]
        assert headings == [title], (identifier, headings)
        return section


def contains_words(block, phrase):
    def words(text):
        return ' '.join(re.findall(r'[^\W_]+', text.casefold()))

    return f' {words(phrase)} ' in f' {words(block.visible())} '


def module_links(block, kind):
    prefix = f'#module-{kind}-'
    return Counter(node.attrs['href'] for node in block.descendants
                   if node.tag == 'a' and node.attrs.get('href', '').startswith(prefix))


def notice_rows(section, title):
    notices = [node for node in section.descendants if node.tag == 'div'
               and any(heading.tag == 'h3' and heading.visible().strip() == title
                       for heading in node.descendants)]
    assert notices, ('missing graph notice', title)
    return [node for node in notices[-1].descendants if node.tag == 'li']


def directive_anchor(kind, unit, directive):
    return f'module-{kind}-{unit}-{directive}'


def candidate_links(entry, declarations):
    return Counter('#' + directive_anchor('declaration', *ref)
                   for ref in candidate_refs(entry, declarations))


def has_class(node, name):
    return name in node.attrs.get('class', '').split()


def change_group(panel, title):
    heading, = (node for node in panel.descendants
                if node.tag == 'h3' and node.visible().strip() == title)
    return heading.parent


def change_side(row, side):
    columns = [node for node in row.descendants if has_class(node, 'module-side') and has_class(node, side)]
    assert len(columns) <= 1, (side, row.visible())
    if columns:
        assert contains_words(columns[0], side), columns[0].visible()
    return columns[0] if columns else None


def check_candidate_sites(block, entry, declarations, *, linked):
    expected_links = candidate_links(entry, declarations) if linked else Counter()
    assert module_links(block, 'declaration') == expected_links, (entry, block.visible())
    links = {node.attrs.get('href'): node for node in block.descendants if node.tag == 'a'}
    for ref in candidate_refs(entry, declarations):
        href = '#' + directive_anchor('declaration', *ref)
        location = site(declarations[ref]['location'])
        assert location in (links[href] if linked else block).visible(), (entry, location, block.visible())


def check_changed_side(block, kind, entry, graph, *, linked):
    units, declarations = graph
    if not linked:
        assert not any(node.tag == 'a' for node in block.descendants), block.visible()
    if kind == 'imports':
        assert contains_words(block, entry['status']), (entry, block.visible())
        href = '#' + directive_anchor('import', entry['unit_id'], entry['directive_index'])
        location = site(entry['location'])
        assert location in block.visible(), (entry, block.visible())
        if linked:
            link, = (node for node in block.descendants if node.tag == 'a' and node.attrs.get('href') == href)
            assert location in link.visible(), (entry, link.visible())
    if kind in ('imports', 'conflicts'):
        check_candidate_sites(block, entry, declarations, linked=linked)
    if kind == 'units':
        assert contains_words(block, entry['scan']), (entry, block.visible())
    if kind in ('units', 'cycles'):
        members = [entry['id']] if kind == 'units' else entry
        assert module_links(block, 'unit') == Counter(f'#module-unit-{unit}' for unit in members if linked)
        for unit in members:
            assert units[unit]['source'] in block.visible(), (unit, block.visible())


def check_module_delta_html(document, before, after, changes):
    panel = document.section('module-changes', 'Module changes')
    if changes.get('matching') == 'legacy_source_only':
        assert contains_words(panel, 'matched by source path'), panel.visible()
    if changes.get('unavailable'):
        assert contains_words(panel, 'Module comparison unavailable'), panel.visible()
        assert 'No semantic module changes.' not in panel.visible()
        assert not any(has_class(node, 'module-change') for node in panel.descendants)
        return panel

    groups = {'imports': 'Import changes', 'units': 'Translation unit changes',
              'conflicts': 'Provider conflict changes', 'cycles': 'Cycle changes'}
    if not (changes.get('completeness') or any(changes[kind] for kind in groups)):
        assert 'No semantic module changes.' in panel.visible(), panel.visible()
        assert not any(has_class(node, 'module-change') for node in panel.descendants)
        return panel

    assert 'No semantic module changes.' not in panel.visible(), panel.visible()
    expected_count = sum(len(changes[kind]) for kind in groups) + int('completeness' in changes)
    assert sum(has_class(node, 'module-change') for node in panel.descendants) == expected_count
    old, new = index_graph(before['module_graph']), index_graph(after['module_graph'])
    for kind, title in groups.items():
        if not changes[kind]:
            continue
        group = change_group(panel, title)
        rows = [node for node in group.descendants if has_class(node, 'module-change')]
        assert len(rows) == len(changes[kind]), (kind, [row.visible() for row in rows], changes[kind])
        for row, change in zip(rows, changes[kind]):
            assert contains_words(row, change['change']), (change, row.visible())
            if kind in ('imports', 'conflicts'):
                assert (change.get('after') or change['before'])['name'] in row.visible(), (change, row.visible())
            for side, graph in (('before', old), ('after', new)):
                block, entry = change_side(row, side), change.get(side)
                assert (block is None) == (entry is None), (change, row.visible())
                if entry is not None:
                    check_changed_side(block, kind, entry, graph, linked=side == 'after')
    if 'completeness' in changes:
        complete = change_group(panel, 'Scan completeness')
        for side in ('before', 'after'):
            expected = 'Complete' if changes['completeness'][side] else 'Incomplete'
            block = change_side(complete, side)
            assert block is not None and contains_words(block, expected), (side, complete.visible())
    return panel


def check_graph_html(document, graph):
    section = document.section('module-graph', 'Module graph')
    assert graph['native_target'] in section.visible(), graph['native_target']
    assert contains_words(section, 'Complete' if graph['complete'] else 'Incomplete'), section.visible()

    units, declarations = index_graph(graph)
    imports = {(entry['unit_id'], entry['directive_index']): entry for entry in graph['imports']}
    expected_anchors = {f'module-unit-{unit}' for unit in units}
    expected_anchors.update(directive_anchor('declaration', *ref) for ref in declarations)
    expected_anchors.update(directive_anchor('import', *ref) for ref in imports)
    anchors = [(node.attrs['id'], node) for node in section.descendants
               if node.attrs.get('id', '').startswith(('module-unit-', 'module-declaration-', 'module-import-'))]
    by_id = dict(anchors)
    assert len(anchors) == len(by_id) and set(by_id) == expected_anchors, [anchor for anchor, _ in anchors]

    for identifier, unit in units.items():
        block = by_id[f'module-unit-{identifier}']
        assert unit['source'] in block.visible(), (unit, block.visible())
        if unit['scan'] != 'complete':
            assert contains_words(block, unit['scan']), (unit, block.visible())
        expected_directives = {directive_anchor(kind, *ref)
                               for kind, rows in (('declaration', declarations), ('import', imports))
                               for ref in rows if ref[0] == identifier}
        observed = {node.attrs.get('id') for node in block.descendants} & expected_anchors
        assert observed == expected_directives, (unit, observed, expected_directives)

    for kind, entries in (('declaration', declarations), ('import', imports)):
        for ref, entry in entries.items():
            record = by_id[directive_anchor(kind, *ref)]
            assert entry['name'] in record.visible() and site(entry['location']) in record.visible(), (
                entry, record.visible())
            spelling = 'export import' if entry['kind'] == 'exported_import' else entry['kind']
            assert contains_words(record, spelling), (entry, record.visible())
            if kind == 'import':
                assert contains_words(record, entry['status']), (entry, record.visible())
                check_candidate_sites(record, entry, declarations, linked=True)

    if graph['conflicts']:
        rows = notice_rows(section, 'Provider conflicts')
        for conflict in graph['conflicts']:
            links = candidate_links(conflict, declarations)
            matching = [row for row in rows if conflict['name'] in row.visible()
                        and module_links(row, 'declaration') == links]
            assert len(matching) == 1, (conflict, [row.visible() for row in rows])

    if graph['cycles']:
        rows = notice_rows(section, 'Cyclic components')
        observed = [module_links(row, 'unit') for row in rows]
        expected = [Counter(f'#module-unit-{unit}' for unit in component) for component in graph['cycles']]
        assert sorted(map(sorted, observed)) == sorted(map(sorted, expected)), (observed, expected)
        assert all(count == 1 for component in observed for count in component.values()), observed


def show_html(ycxx, scope, env, journal, triple, plan, *args):
    document = PlanHTML(show(ycxx, scope, env, journal, triple, *args, format_='html'))
    document.section('facts', 'Plan facts')
    if 'module_graph' in plan:
        check_graph_html(document, plan['module_graph'])
    else:
        assert not document.sections('module-graph'), 'HTML showed a graph for an unresolved plan'
    if '--against' not in args:
        assert not document.sections('module-changes'), 'HTML showed changes without a baseline'
    return document


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


def save_comparison(scope, before):
    snapshot = scope.parent / 'graph-comparison.json'
    snapshot.write_text(json.dumps(before), encoding='utf-8')
    return snapshot


def compare(ycxx, scope, env, journal, triple, before):
    return show(ycxx, scope, env, journal, triple, '--against', str(save_comparison(scope, before)))


def compare_html(ycxx, scope, env, journal, triple, before, after, diff):
    snapshot = save_comparison(scope, before)
    saved = snapshot.read_bytes(), snapshot.stat().st_mtime_ns
    document = show_html(ycxx, scope, env, journal, triple, after, '--against', str(snapshot))
    assert (snapshot.read_bytes(), snapshot.stat().st_mtime_ns) == saved, 'comparison changed the snapshot'
    document.section('changes', 'Plan changes')
    if 'module_changes' in diff:
        check_module_delta_html(document, before, after, diff['module_changes'])
    else:
        assert not document.sections('module-changes'), 'legacy plan without a graph showed module changes'
    return document


def check_module_changes(diff, *, metadata=None, **groups):
    assert diff['schema'] == 1 and diff['target'] == 'app' and isinstance(diff['changes'], list), diff
    assert 'module_changes' in diff, 'comparison of two module graphs omitted module_changes'
    changes = diff['module_changes']
    metadata = {} if metadata is None else metadata
    kinds = {'units', 'imports', 'conflicts', 'cycles'}
    assert groups.keys() <= kinds, groups
    assert set(changes) == kinds | metadata.keys(), changes
    assert {key: changes[key] for key in metadata} == metadata, changes
    for key in kinds:
        rows = [(row['change'], row.get('before'), row.get('after')) for row in changes[key]]
        assert Counter(json.dumps(row, sort_keys=True) for row in rows) == Counter(
            json.dumps(row, sort_keys=True) for row in groups.get(key, ())), (key, rows, groups.get(key, ()))


def legacy_graph(plan):
    legacy = deepcopy(plan)
    for unit in legacy['module_graph']['units']:
        unit.pop('configuration')
    return legacy


def renumber_graph(plan):
    renumbered = deepcopy(plan)
    graph = renumbered['module_graph']
    ids = {unit['id']: 100 + 3 * i for i, unit in enumerate(reversed(graph['units']))}
    for unit in graph['units']:
        unit['id'] = ids[unit['id']]
        for declaration in unit['declarations']:
            declaration['directive_index'] += 10
    for entry in graph['imports']:
        entry['unit_id'] = ids[entry['unit_id']]
        entry['directive_index'] += 10
    for entry in (*graph['imports'], *graph['conflicts']):
        for candidate in entry['candidates']:
            candidate['unit_id'] = ids[candidate['unit_id']]
            candidate['directive_index'] += 10
    graph['cycles'] = [[ids[unit] for unit in reversed(cycle)] for cycle in graph['cycles']]
    return renumbered


def check_invalid_graph(ycxx, scope, env, journal, plan):
    invalid = renumber_graph(plan)
    provider = next(entry['candidates'][0] for entry in invalid['module_graph']['imports'] if entry['candidates'])
    provider['unit_id'] += 1000
    snapshot = scope.parent / 'invalid-module-graph.json'
    snapshot.write_text(json.dumps(invalid), encoding='utf-8')
    before = state(scope), journal_contents(journal)
    for format_ in ('json', 'html'):
        result = run_client(ycxx, scope, env, 'show', 'app', f'--target={LINUX}', f'--format={format_}',
                            '--against', str(snapshot))
        assert result.returncode != 0 and 'saved module graph' in result.stderr.lower(), result
        assert (state(scope), journal_contents(journal)) == before, 'comparison wrote state or ran a tool'


def unit_for_source(graph, source):
    unit, = (unit for unit in graph['units'] if unit['source'] == source)
    return unit


def platform_changes(old, new):
    before, = (entry for entry in old['imports'] if entry['name'] == CHOICE)
    after, = (entry for entry in new['imports'] if entry['name'] == CHOICE)
    units = [('changed', unit_for_source(old, name), unit_for_source(new, name)) for name in CHOICES.values()]
    return {'units': units, 'imports': [('changed', before, after)]}


def check_target_comparison(ycxx, scope, env, journal, linux, darwin):
    old, new = linux['module_graph'], darwin['module_graph']
    diff = compare(ycxx, scope, env, journal, DARWIN, linux)
    check_module_changes(diff, **platform_changes(old, new))

    document = compare_html(ycxx, scope, env, journal, DARWIN, linux, darwin, diff)

    without_graph = {key: value for key, value in linux.items() if key != 'module_graph'}
    legacy = compare(ycxx, scope, env, journal, DARWIN, without_graph)
    assert legacy == {key: value for key, value in diff.items() if key != 'module_changes'}, legacy
    legacy_document = compare_html(ycxx, scope, env, journal, DARWIN, without_graph, darwin, legacy)
    assert legacy_document.section('changes', 'Plan changes').visible() == document.section(
        'changes', 'Plan changes').visible()
    legacy = legacy_graph(linux)
    legacy_diff = compare(ycxx, scope, env, journal, DARWIN, legacy)
    check_module_changes(legacy_diff, metadata={'matching': 'legacy_source_only'},
                         **platform_changes(legacy['module_graph'], new))
    compare_html(ycxx, scope, env, journal, DARWIN, legacy, darwin, legacy_diff)


def check_diamond_html(ycxx, scope, env, journal):
    arms = ('left.cc', 'right.cc')
    for arm in arms:
        replace_source(scope, arm, SOURCES[arm].replace('import beam.bottom;', f'import {CHOICE};'))
    linux = renumber_graph(show(ycxx, scope, env, journal, LINUX))
    darwin = show(ycxx, scope, env, journal, DARWIN)
    old, new = linux['module_graph'], darwin['module_graph']

    def imports_from(plan):
        return {edge['location']['path']: edge for edge in plan['imports'] if edge['name'] == CHOICE}

    before, after = imports_from(old), imports_from(new)
    assert before.keys() == after.keys() == {'main.cc', *arms}, (before, after)
    diff = compare(ycxx, scope, env, journal, DARWIN, linux)
    check_module_changes(
        diff,
        units=[('changed', unit_for_source(old, name), unit_for_source(new, name)) for name in CHOICES.values()],
        imports=[('changed', before[arm], after[arm]) for arm in before],
    )
    compare_html(ycxx, scope, env, journal, DARWIN, linux, darwin, diff)

    # Changing a saved site preserves provider identity but exercises the old-side HTML join.
    original_provider = unit_for_source(old, CHOICES[LINUX])['declarations'][0]
    original_provider['location']['path'] = UNSAFE_SNAPSHOT_SITE
    document = compare_html(ycxx, scope, env, journal, DARWIN, linux, darwin, diff)
    assert UNSAFE_SNAPSHOT_SITE not in document.document
    assert not any(node.attrs.get('data-module-probe') == '1' for node in document.elements)
    for arm in arms:
        replace_source(scope, arm, SOURCES[arm])


def check_partial_comparison(ycxx, scope, env, journal, complete, partial, source, *, added=False):
    old, new = complete['module_graph'], partial['module_graph']
    old_unit = None if added else unit_for_source(old, source)
    units = [('added' if added else 'changed', old_unit, unit_for_source(new, source))]
    partial_status = {'resolved': 'provisional', 'external_or_unresolved': 'pending',
                      'unresolved_partition': 'pending'}
    old_imports = {(entry['unit_id'], entry['directive_index']): entry for entry in old['imports']}
    new_imports = {(entry['unit_id'], entry['directive_index']): entry for entry in new['imports']}
    assert old_imports.keys() == new_imports.keys(), new_imports
    imports = []
    for key, before in old_imports.items():
        after = new_imports[key]
        assert after['status'] == partial_status.get(before['status'], before['status']), (before, after)
        if before['status'] != after['status']:
            imports.append(('changed', before, after))
    diff = compare(ycxx, scope, env, journal, LINUX, complete)
    check_module_changes(diff, units=units, imports=imports,
                         metadata={'completeness': {'change': 'changed', 'before': True, 'after': False}})
    compare_html(ycxx, scope, env, journal, LINUX, complete, partial, diff)


def check_conflict_and_cycle_comparison(ycxx, scope, env, journal, before):
    edits = {
        'duplicate_z.cc': 'export module former.duplicate;\n',
        'self.cc': 'export module cycle.single;\n',
        'bottom.cc': SOURCES['bottom.cc'] + 'import beam.bottom;\n',
    }
    for source, text in edits.items():
        replace_source(scope, source, text)
    after = show(ycxx, scope, env, journal, LINUX)
    old, new = before['module_graph'], after['module_graph']
    old_duplicate = [entry for entry in old['imports'] if entry['name'] == 'duplicate.example']
    new_duplicate = [entry for entry in new['imports'] if entry['name'] == 'duplicate.example']
    assert len(old_duplicate) == len(new_duplicate) == 2, (old_duplicate, new_duplicate)
    assert {entry['status'] for entry in old_duplicate} == {'ambiguous'}, old_duplicate
    assert {entry['status'] for entry in new_duplicate} == {'resolved'}, new_duplicate
    added, = (entry for entry in new['imports'] if entry['unit_id'] == unit_for_source(new, 'bottom.cc')['id'])
    removed, = (entry for entry in old['imports'] if entry['unit_id'] == unit_for_source(old, 'self.cc')['id'])
    assert (added['name'], added['status'], removed['name']) == ('beam.bottom', 'resolved', 'cycle.single')
    conflict, = (entry for entry in old['conflicts'] if entry['name'] == 'duplicate.example')
    assert {entry['name'] for entry in new['conflicts']} == {'unused.clash'}, new
    added_cycle, = (cycle for cycle in new['cycles'] if cycle == [added['unit_id']])
    removed_cycle, = (cycle for cycle in old['cycles'] if cycle == [removed['unit_id']])
    reciprocal_imports = {unit_for_source(new, source)['id'] for source in ('left.cc', 'duplicate_a.cc')}
    unblocked_cycle, = (cycle for cycle in new['cycles'] if set(cycle) == reciprocal_imports)
    diff = compare(ycxx, scope, env, journal, LINUX, before)
    check_module_changes(
        diff,
        units=[('changed', unit_for_source(old, 'duplicate_z.cc'), unit_for_source(new, 'duplicate_z.cc'))],
        imports=[('changed', first, second) for first, second in zip(old_duplicate, new_duplicate)]
        + [('added', None, added), ('removed', removed, None)],
        conflicts=[('removed', conflict, None)],
        cycles=[('added', None, added_cycle), ('added', None, unblocked_cycle), ('removed', removed_cycle, None)],
    )
    compare_html(ycxx, scope, env, journal, LINUX, before, after, diff)
    for source in edits:
        replace_source(scope, source, SOURCES[source])
    reverse = compare(ycxx, scope, env, journal, LINUX, after)
    check_module_changes(
        reverse,
        units=[('changed', unit_for_source(new, 'duplicate_z.cc'), unit_for_source(old, 'duplicate_z.cc'))],
        imports=[('changed', second, first) for first, second in zip(old_duplicate, new_duplicate)]
        + [('removed', added, None), ('added', None, removed)],
        conflicts=[('added', None, conflict)],
        cycles=[('removed', added_cycle, None), ('removed', unblocked_cycle, None), ('added', None, removed_cycle)],
    )
    compare_html(ycxx, scope, env, journal, LINUX, after, before, reverse)


def check_edits(ycxx, scope, env, journal, before):
    legacy = {key: value for key, value in before.items() if key not in ('module_graph', 'dependency_graph')}
    saved = scope.parent / 'old-schema-1.json'
    saved.write_text(json.dumps(legacy), encoding='utf-8')
    current_snapshot = scope.parent / 'with-module-graph.json'
    current_snapshot.write_text(json.dumps(before), encoding='utf-8')
    diff = show(ycxx, scope, env, journal, LINUX, '--against', str(saved))
    assert diff == {'schema': 1, 'target': 'app', 'changes': []}, diff
    check_module_changes(show(ycxx, scope, env, journal, LINUX, '--against', str(current_snapshot)))
    check_module_changes(compare(ycxx, scope, env, journal, LINUX, renumber_graph(before)))
    check_invalid_graph(ycxx, scope, env, journal, before)
    html = show_html(ycxx, scope, env, journal, LINUX, before, '--against', str(saved))
    assert 'No changes.' in html.section('changes', 'Plan changes').visible()

    replace_source(scope, 'linux.cc', SOURCES['linux.cc'].replace('return 11;', 'return 12;'))
    summary, events = build(ycxx, scope, env, LINUX, 'body-only edit retains the structured graph')
    check_provider_reuse(summary, events)
    assert show(ycxx, scope, env, journal, LINUX) == before
    check_module_changes(show(ycxx, scope, env, journal, LINUX, '--against', str(current_snapshot)))

    shifted = '\n\n' + SOURCES['main.cc']
    replace_source(scope, 'main.cc', shifted)
    moved = show(ycxx, scope, env, journal, LINUX)
    require_moved = deepcopy(before['module_graph'])
    for entry in require_moved['imports']:
        if entry['location']['path'] == 'main.cc':
            entry['location']['line'] += 2
    assert check_graph(scope, moved, LINUX) == require_moved, moved
    diff = show(ycxx, scope, env, journal, LINUX, '--against', str(current_snapshot))
    assert diff['changes'] == [], diff
    check_module_changes(diff)
    compare_html(ycxx, scope, env, journal, LINUX, before, moved, diff)
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
    old_module, = (entry for entry in before['module_graph']['imports'] if entry['name'] == EXTERNAL)
    module_diff = show(ycxx, scope, env, journal, LINUX, '--against', str(current_snapshot))
    assert module_diff['changes'] == diff, module_diff
    check_module_changes(module_diff, imports=[('removed', old_module, None)])
    html = show_html(ycxx, scope, env, journal, LINUX, reduced, '--against', str(saved))
    changes = html.section('changes', 'Plan changes')
    assert contains_words(changes, 'removed') and EXTERNAL in changes.visible(), changes.visible()

    # The surviving identical import pairs with the first original occurrence.
    replace_source(scope, 'main.cc', shifted.replace('import beam.top;', '// removed the first copy', 1))
    first, second = (entry for entry in before['module_graph']['imports'] if entry['name'] == 'beam.top')
    assert first['directive_index'] < second['directive_index'], (first, second)
    check_module_changes(show(ycxx, scope, env, journal, LINUX, '--against', str(current_snapshot)),
                         imports=[('removed', second, None)])
    replace_source(scope, 'main.cc', shifted)


def check_project(ycxx, scope, env, journal):
    cold = show(ycxx, scope, env, journal, LINUX)
    assert facts(cold, 'unresolved') and 'module_graph' not in cold and not (scope / '.ycxx').exists(), cold
    show_html(ycxx, scope, env, journal, LINUX, cold)

    build(ycxx, scope, env, LINUX, 'resolve the Linux project')
    before = show(ycxx, scope, env, journal, LINUX)
    check_graph(scope, before, LINUX)
    show_html(ycxx, scope, env, journal, LINUX, before)
    daemon = daemon_identity(ycxx, scope, env)
    assert daemon is not None
    build(ycxx, scope, env, DARWIN, 'resolve the Darwin project')
    darwin = show(ycxx, scope, env, journal, DARWIN)
    check_graph(scope, darwin, DARWIN)
    show_html(ycxx, scope, env, journal, DARWIN, darwin)
    assert show(ycxx, scope, env, journal, LINUX) == before
    check_target_comparison(ycxx, scope, env, journal, before, darwin)
    check_diamond_html(ycxx, scope, env, journal)

    check_edits(ycxx, scope, env, journal, before)
    complete = show(ycxx, scope, env, journal, LINUX)
    check_conflict_and_cycle_comparison(ycxx, scope, env, journal, complete)
    replace_source(scope, 'darwin.cc', '#include "missing-fixture.h"\n' + SOURCES['darwin.cc'])
    partial = show(ycxx, scope, env, journal, LINUX)
    check_graph(scope, partial, LINUX, incomplete='darwin.cc', reason='preprocessing_incomplete')
    show_html(ycxx, scope, env, journal, LINUX, partial)
    check_partial_comparison(ycxx, scope, env, journal, complete, partial, 'darwin.cc')
    replace_source(scope, 'darwin.cc', SOURCES['darwin.cc'])

    replace_source(scope, 'build.ycxx', definition(journal, generated=True))
    _, events = build(ycxx, scope, env, LINUX, 'admit a generated module source')
    check_external_actions(events, GENERATOR)
    generated = str(namespace(LINUX) / 'virtual' / GENERATOR / GENERATED)
    partial = show(ycxx, scope, env, journal, LINUX)
    check_graph(scope, partial, LINUX, incomplete=generated, reason='generated_inputs_unavailable')
    show_html(ycxx, scope, env, journal, LINUX, partial)
    check_partial_comparison(ycxx, scope, env, journal, complete, partial, generated, added=True)

    build(ycxx, scope, env, DARWIN, 'admit the same generated module source for Darwin')
    generated_darwin = str(namespace(DARWIN) / 'virtual' / GENERATOR / GENERATED)
    darwin = show(ycxx, scope, env, journal, DARWIN)
    check_graph(scope, darwin, DARWIN, incomplete=generated_darwin, reason='generated_inputs_unavailable')
    old, new = partial['module_graph'], darwin['module_graph']
    linux_unit = unit_for_source(old, generated)
    darwin_unit = unit_for_source(new, generated_darwin)
    logical_source = str(namespace(None) / 'virtual' / GENERATOR / GENERATED)
    assert linux_unit['configuration'] == darwin_unit['configuration'], (linux_unit, darwin_unit)
    assert linux_unit['configuration']['source'] == logical_source, linux_unit
    check_module_changes(compare(ycxx, scope, env, journal, DARWIN, partial), **platform_changes(old, new))
    assert journal_contents(journal) == 'generated\n' and daemon_identity(ycxx, scope, env) == daemon


def check_configurations(ycxx, scope, env, journal):
    scope.mkdir()
    (scope / 'main.cc').write_text('int main() { return 0; }\n', encoding='utf-8')
    # Discovery precedes semantic checks. The preceding import makes the first declaration's
    # directive index differ from its offset in the declarations array.
    (scope / UNSAFE_SOURCE).write_text('''\
module;
#include <config.hh>
#if SIDE == 1
import external.first;
export module cycle.left;
import cycle.left;
#else
export module cycle.right;
#ifndef DISABLE_RIGHT_CYCLE
import cycle.right;
#endif
#endif
''', encoding='utf-8')
    for directory, side in (('left-include', 1), ('right-include', 2)):
        (scope / directory).mkdir()
        (scope / directory / 'config.hh').write_text(f'#define SIDE {side}\n', encoding='utf-8')
    configuration_manifest = f'''\
cpp.library "left" {{ sources = ["{UNSAFE_SOURCE}"] public_include_dirs = ["left-include", "right-include"] }}
cpp.library "right" {{ sources = ["{UNSAFE_SOURCE}"] public_include_dirs = ["right-include", "left-include"] }}
cpp.library "again" {{ sources = ["{UNSAFE_SOURCE}"] public_include_dirs = ["left-include", "right-include"] }}
cpp.executable "app" {{ sources = ["main.cc"] deps = ["again", "left", "right"] }}
'''
    (scope / 'build.ycxx').write_text(configuration_manifest, encoding='utf-8')
    build(ycxx, scope, env, LINUX, 'resolve repeated and distinct configured units')
    plan = show(ycxx, scope, env, journal, LINUX)
    graph = module_graph(plan, LINUX)
    units, declarations = index_graph(graph)
    shared = [unit for unit in units.values() if unit['source'] == UNSAFE_SOURCE]
    assert len(shared) == 2 and len(units) == 3 and graph['complete'], units
    assert max(units) + 1 > len(units), units
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
    html = show_html(ycxx, scope, env, journal, LINUX, plan)
    assert UNSAFE_SOURCE not in html.document, 'the source name was embedded as unescaped HTML'

    configurations = {unit['declarations'][0]['name']: unit['configuration'] for unit in shared}
    assert configurations == {
        'cycle.left': {'source': UNSAFE_SOURCE, 'include_dirs': ['left-include', 'right-include']},
        'cycle.right': {'source': UNSAFE_SOURCE, 'include_dirs': ['right-include', 'left-include']},
    }, configurations
    # Keep the first dependency fixed so the application's inherited include order stays stable.
    reordered = configuration_manifest.replace('["again", "left", "right"]', '["again", "right", "left"]')
    replace_source(scope, 'build.ycxx', reordered)
    build(ycxx, scope, env, LINUX, 'reorder the repeated and distinct configured units')
    reordered_plan = show(ycxx, scope, env, journal, LINUX)
    current = module_graph(reordered_plan, LINUX)
    before_ids = {unit['declarations'][0]['name']: unit['id'] for unit in shared}
    after_ids = {unit['declarations'][0]['name']: unit['id'] for unit in current['units']
                 if unit['source'] == UNSAFE_SOURCE}
    assert before_ids.keys() == after_ids.keys() and before_ids != after_ids, (before_ids, after_ids)

    diff = compare(ycxx, scope, env, journal, LINUX, plan)
    check_module_changes(diff)
    compare_html(ycxx, scope, env, journal, LINUX, plan, reordered_plan, diff)
    legacy = legacy_graph(plan)
    legacy_diff = compare(ycxx, scope, env, journal, LINUX, legacy)
    assert legacy_diff['changes'] == diff['changes'], legacy_diff
    check_module_changes(legacy_diff, metadata={'unavailable': 'ambiguous_legacy_configuration'})
    compare_html(ycxx, scope, env, journal, LINUX, legacy, reordered_plan, legacy_diff)
    check_invalid_graph(ycxx, scope, env, journal, legacy)

    replace_source(scope, 'build.ycxx', reordered.replace('"left-include"', '"left-include/"'))
    build(ycxx, scope, env, LINUX, 'add a trailing separator to an include directory')
    check_module_changes(compare(ycxx, scope, env, journal, LINUX, reordered_plan))

    right_import, = (entry for entry in current['imports'] if entry['name'] == 'cycle.right')
    right_cycle, = (cycle for cycle in current['cycles'] if cycle == [right_import['unit_id']])
    replace_source(scope, 'right-include/config.hh', '#define SIDE 2\n#define DISABLE_RIGHT_CYCLE\n')
    check_module_changes(compare(ycxx, scope, env, journal, LINUX, reordered_plan),
                         imports=[('removed', right_import, None)], cycles=[('removed', right_cycle, None)])


def check_empty_graph(ycxx, scope, env, journal):
    replace_source(scope, 'build.ycxx', 'cpp.executable "app" { sources = ["main.cc"] }\n')
    traced_build(ycxx, scope, env, 'resolve a host-default project without modules')
    (scope / executable(None)).unlink()
    host = {'linux': LINUX, 'darwin': DARWIN}[sys.platform]
    plan = show(ycxx, scope, env, journal, None)
    graph = module_graph(plan, host)
    units, declarations = index_graph(graph)
    unit, = units.values()
    assert unit['source'] == 'main.cc' and graph['complete'] and not declarations, graph
    assert graph['imports'] == graph['conflicts'] == graph['cycles'] == [], graph
    show_html(ycxx, scope, env, journal, None, plan)
    diff = compare(ycxx, scope, env, journal, None, plan)
    check_module_changes(diff)
    compare_html(ycxx, scope, env, journal, None, plan, plan, diff)


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
