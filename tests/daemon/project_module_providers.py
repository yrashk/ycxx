"""Verify configured module provider edges, cycles, and semantic reuse (#1353)."""

import html
import json
from pathlib import Path
import sys
import tempfile

from project_build import daemon_identity, env_for, run_client, traced_build
from project_build_watch import replace_source
from project_module_discovery import trace_events
from project_shader_bundle import check_external_actions
from project_targets import DARWIN, LINUX, executable, journal_contents, namespace, target_args
from runtime_dir import runtime_root


CHOICE = 'platform.choice'
EXTERNAL = 'vendor.external'
GENERATOR = 'generated'
GENERATED = 'provider.cc'
CHOICES = {LINUX: 'linux.cc', DARWIN: 'darwin.cc'}
SOURCES = {
    'main.cc': f'''\
#line 500 "logical-main.cc"
import {CHOICE};
import beam.top;
import beam.top;
import {EXTERNAL};
import duplicate.example;
int main() {{ return 7; }}
''',
    'linux.cc': f'''\
#if defined(__x86_64__)
#line 700 "logical-linux.cc"
export module {CHOICE};
export int selected() {{ return 11; }}
#endif
''',
    'darwin.cc': f'''\
#if defined(__aarch64__)
#line 800 "logical-darwin.cc"
export module {CHOICE};
export int selected() {{ return 21; }}
#endif
''',
    'top.cc': 'export module beam.top;\nexport import beam.left;\nimport beam.right;\nimport :detail;\nimport :missing;\n',
    'right.cc': 'export module beam.right;\nimport beam.bottom;\n',
    'left.cc': 'export module beam.left;\nimport beam.bottom;\nimport duplicate.example;\n',
    'bottom.cc': 'export module beam.bottom;\n',
    'implementation.cc': 'module beam.bottom;\n',
    'partition.cc': 'module beam.top:detail;\n',
    'duplicate_z.cc': 'export module duplicate.example;\n',
    'duplicate_a.cc': 'export module duplicate.example;\nimport beam.left;\n',
    'unused_z.cc': 'export module unused.clash;\n',
    'unused_a.cc': 'export module unused.clash;\n',
    'self.cc': 'export module cycle.single;\nimport cycle.single;\n',
    'cycle_c.cc': 'export module cycle.c;\nimport cycle.a;\n',
    'cycle_a.cc': 'export module cycle.a;\nimport cycle.b;\n',
    'cycle_b.cc': 'export module cycle.b;\nimport cycle.c;\n',
}
EXPECTED_CYCLES = {frozenset(('self.cc',)), frozenset(('cycle_a.cc', 'cycle_b.cc', 'cycle_c.cc'))}
CONFLICTS = {'duplicate.example': ('duplicate_a.cc', 'duplicate_z.cc'),
             'unused.clash': ('unused_a.cc', 'unused_z.cc')}
GENERATOR_SCRIPT = rf'''
printf 'generated\n' >> "$1" || exit 1
printf 'export module {EXTERNAL};\n' > "$2"
'''


def definition(journal, *, generated=False):
    inputs = [json.dumps(path) for path in SOURCES]
    producer = ''
    if generated:
        producer = f'''\
generate "{GENERATOR}" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(GENERATOR_SCRIPT)}, "provider", {json.dumps(str(journal))}, output("source")]
    outputs = {{ source = "{GENERATED}" }}
}}
'''
        inputs.append(f'artifact("{GENERATOR}", "source")')
    return producer + f'''\
cpp.executable "app" {{
    sources = [{", ".join(inputs)}]
}}
'''


def prepare(scope, journal):
    scope.mkdir()
    for path, text in SOURCES.items():
        (scope / path).write_text(text, encoding='utf-8')
    (scope / 'build.ycxx').write_text(definition(journal), encoding='utf-8')


def state(scope):
    root = scope / '.ycxx'
    entries = {}
    if root.exists():
        for path in (root, *root.rglob('*')):
            status = path.stat()
            entries[str(path.relative_to(root))] = (status.st_ino, status.st_size, status.st_mtime_ns)
    return entries


def show(ycxx, scope, env, journal, triple, *args, format_='json'):
    before = state(scope), journal_contents(journal)
    result = run_client(ycxx, scope, env, 'show', 'app', *target_args(triple), f'--format={format_}', *args)
    assert result.returncode == 0, (args, result.stderr)
    assert (state(scope), journal_contents(journal)) == before, 'show wrote state or launched a tool'
    assert not (scope / executable(triple)).exists(), 'inspection published an executable'
    return json.loads(result.stdout) if format_ == 'json' else result.stdout


def facts(plan, kind):
    return [item for item in plan['facts'] if item['kind'] == kind]


def sites(scope, path, line_spellings):
    lines = (scope / path).read_text(encoding='utf-8').splitlines()
    return [f'{path}:{line}:{len(text) - len(text.lstrip()) + 1}'
            for line, text in enumerate(lines, 1) if text.strip() in line_spellings]


def displayed_paths(value):
    return [path.strip('"') for path in value.split(', ')]


def check_edge(scope, imports, edges, importer, name, candidates=(), *, partial=False, spelling=None):
    spelling = spelling or name
    imports_at = sites(scope, importer, {f'import {spelling};', f'export import {spelling};'})
    rows = [item for item in imports if item['value'] == name and item['origin'].startswith(importer + ':')]
    assert {item['origin'] for item in rows} == set(imports_at) and len(rows) == len(imports_at), rows
    assert imports_at, (importer, name)

    for directive in rows:
        edge = edges[directive['name']]
        assert edge['target'] == 'app', edge
        if not candidates:
            status = 'no known provider in partial graph' if partial else (
                'unresolved partition' if ':' in name else 'external or unresolved')
            assert edge['value'] == status and edge['origin'] == directive['origin'], edge
            continue
        status = 'ambiguous' if len(candidates) > 1 else ('resolved in partial graph' if partial else 'resolved')
        label, paths = edge['value'].split(' · ', 1)
        observed = displayed_paths(paths)
        assert label == status and sorted(observed) == sorted(candidates), edge
        locations = []
        for path in observed:
            location, = sites(scope, path, {f'module {name};', f'export module {name};'})
            locations.append(location)
        assert edge['origin'] == f'{directive["origin"]} → {", ".join(locations)}', edge
    return len(rows)


def check_plan(scope, plan, triple, *, partial=False):
    assert (plan['schema'], plan['target']) == (1, 'app'), plan
    imports = facts(plan, 'module-import')
    providers = facts(plan, 'module-provider')
    edges = {row['name']: row for row in providers}
    assert len(edges) == len(providers) == len(imports), providers
    assert set(edges) == {row['name'] for row in imports}, providers
    pending = facts(plan, 'module-resolution-pending')
    assert len(pending) == int(partial), pending
    if partial:
        assert pending[0]['value'] == 'provider resolution is provisional', pending

    expected_imports = 0
    for source, module, candidates, spelling in (
        ('main.cc', CHOICE, (CHOICES[triple],), None),
        ('main.cc', 'beam.top', ('top.cc',), None),
        ('main.cc', EXTERNAL, (), None),
        ('main.cc', 'duplicate.example', CONFLICTS['duplicate.example'], None),
        ('top.cc', 'beam.left', ('left.cc',), None),
        ('top.cc', 'beam.right', ('right.cc',), None),
        ('top.cc', 'beam.top:detail', ('partition.cc',), ':detail'),
        ('top.cc', 'beam.top:missing', (), ':missing'),
        ('left.cc', 'beam.bottom', ('bottom.cc',), None),
        ('right.cc', 'beam.bottom', ('bottom.cc',), None),
        ('left.cc', 'duplicate.example', CONFLICTS['duplicate.example'], None),
        ('duplicate_a.cc', 'beam.left', ('left.cc',), None),
        ('self.cc', 'cycle.single', ('self.cc',), None),
        ('cycle_a.cc', 'cycle.b', ('cycle_b.cc',), None),
        ('cycle_b.cc', 'cycle.c', ('cycle_c.cc',), None),
        ('cycle_c.cc', 'cycle.a', ('cycle_a.cc',), None),
    ):
        expected_imports += check_edge(scope, imports, edges, source, module, candidates,
                                       partial=partial, spelling=spelling)
    assert len(imports) == expected_imports, imports

    conflicts = facts(plan, 'module-provider-conflict')
    assert len(conflicts) == len(CONFLICTS) and {row['name'] for row in conflicts} == set(CONFLICTS), conflicts
    for conflict in conflicts:
        label, paths = conflict['value'].split(' · ', 1)
        assert label == 'ambiguous' and sorted(displayed_paths(paths)) == sorted(CONFLICTS[conflict['name']]), conflict
    cycles = facts(plan, 'module-cycle')
    assert len(cycles) == len(EXPECTED_CYCLES), cycles
    assert {frozenset(displayed_paths(cycle['name'])) for cycle in cycles} == EXPECTED_CYCLES, cycles
    for cycle in cycles:
        members = displayed_paths(cycle['name'])
        assert members == sorted(members), cycle
        status = 'cycle among known project units (partial graph)' if partial else 'cycle among project units'
        assert cycle['value'] == status, cycle
    return providers, cycles


def reports(events):
    semantic = trace_events(events, 'ModuleProviders')
    physical = trace_events(events, 'ModuleProviderReport')
    assert semantic and physical, 'build did not trace module provider resolution and its source report'
    return semantic, physical[-1]


def build(ycxx, scope, env, triple, label):
    summary, events = traced_build(ycxx, scope, env, label, *target_args(triple), succeeds=False)
    assert summary['diagnostics'], summary
    assert not (scope / executable(triple)).exists(), 'failed semantic compilation published an executable'
    return summary, events


def check_incremental(ycxx, scope, env, journal, before, original):
    saved = scope.parent / 'before.json'
    saved.write_text(json.dumps(before), encoding='utf-8')
    old_facts = check_plan(scope, before, LINUX)
    replace_source(scope, 'linux.cc', SOURCES['linux.cc'].replace('return 11;', 'return 12;'))
    summary, events = build(ycxx, scope, env, LINUX, 'body-only edit preserves provider resolution')
    assert summary['actions_executed'] == 0, summary
    assert any(event['outcome'] == 'cutoff' for event in trace_events(events, 'ModuleDirectives', 'linux.cc')), events
    semantic, physical = reports(events)
    assert {event['outcome'] for event in semantic} <= {'verified', 'reused', 'durable'}, semantic
    assert physical['value'] == original['value'], physical
    current = show(ycxx, scope, env, journal, LINUX)
    assert check_plan(scope, current, LINUX) == old_facts

    replace_source(scope, 'main.cc', '\n\n' + SOURCES['main.cc'])
    moved = show(ycxx, scope, env, journal, LINUX)
    assert check_plan(scope, moved, LINUX) != old_facts
    assert show(ycxx, scope, env, journal, LINUX, '--against', str(saved))['changes'] == []
    summary, events = build(ycxx, scope, env, LINUX, 'moving imports changes sites but preserves edges')
    assert summary['actions_executed'] == 0, summary
    semantic, physical = reports(events)
    assert {event['outcome'] for event in semantic} <= {'verified', 'reused', 'durable'}, semantic
    assert 'main.cc:4:1' in physical['value'] and physical['value'] != original['value'], physical


def check_builds(ycxx, scope, env, journal):
    cold = show(ycxx, scope, env, journal, LINUX)
    assert not facts(cold, 'module-provider') and not (scope / '.ycxx').exists(), cold

    _, events = build(ycxx, scope, env, LINUX, 'Linux provider resolution')
    semantic, linux = reports(events)
    assert any(event['outcome'] == 'executed' for event in semantic), semantic
    assert 'main.cc:2:1' in linux['value'] and 'linux.cc:3:1' in linux['value'], linux
    before = show(ycxx, scope, env, journal, LINUX)
    check_plan(scope, before, LINUX)
    daemon = daemon_identity(ycxx, scope, env)
    assert daemon is not None

    _, events = build(ycxx, scope, env, DARWIN, 'Darwin provider resolution')
    _, darwin = reports(events)
    assert 'darwin.cc:3:1' in darwin['value'] and 'linux.cc:3:1' not in darwin['value'], darwin
    check_plan(scope, show(ycxx, scope, env, journal, DARWIN), DARWIN)
    assert show(ycxx, scope, env, journal, LINUX) == before
    document = html.unescape(show(ycxx, scope, env, journal, DARWIN, format_='html'))
    assert all(part in document for part in ('module-provider', 'darwin.cc:3:1', 'ambiguous', 'cycle among project units')), document

    check_incremental(ycxx, scope, env, journal, before, linux)
    replace_source(scope, 'build.ycxx', definition(journal, generated=True))
    show(ycxx, scope, env, journal, LINUX)
    _, events = build(ycxx, scope, env, LINUX, 'the live graph sees the generated provider')
    check_external_actions(events, GENERATOR)
    _, physical = reports(events)
    generated_path = str(namespace(LINUX) / 'virtual' / GENERATOR / GENERATED)
    assert f'{generated_path}:1:1' in physical['value'], physical
    partial = show(ycxx, scope, env, journal, LINUX)
    assert len(facts(partial, 'module-scan-pending')) == 1, partial
    check_plan(scope, partial, LINUX, partial=True)
    assert journal_contents(journal) == 'generated\n' and daemon_identity(ycxx, scope, env) == daemon


def check_provider_path_diff(ycxx, scope, env, journal):
    scope.mkdir()
    first = ('a.cc', 'b.cc, c.cc')
    second = ('a.cc, b.cc', 'c.cc')
    interface = 'export module m;\n'
    inputs = json.dumps(['main.cc', *first, *second])
    (scope / 'build.ycxx').write_text(f'cpp.executable "app" {{\n    sources = {inputs}\n}}\n',
                                     encoding='utf-8')
    (scope / 'main.cc').write_text('import m;\nint main() { return 0; }\n', encoding='utf-8')
    for path in (*first, *second):
        (scope / path).write_text(interface if path in first else '', encoding='utf-8')

    build(ycxx, scope, env, LINUX, 'initial ambiguous providers with commas in their paths')
    before = show(ycxx, scope, env, journal, LINUX)
    snapshot = scope.parent / 'before-provider-paths.json'
    snapshot.write_text(json.dumps(before), encoding='utf-8')
    # Both candidate pairs have the same spelling when joined with comma-space.
    for old, new in zip(first, second):
        replace_source(scope, old, '')
        replace_source(scope, new, interface)
    after = show(ycxx, scope, env, journal, LINUX)
    for plan, paths in ((before, first), (after, second)):
        units = facts(plan, 'module-unit')
        assert {row['origin'] for row in units if row['value'] == 'm'} == {
            f'{path}:1:1' for path in paths
        }, units

    changes = show(ycxx, scope, env, journal, LINUX, '--against', str(snapshot))['changes']
    for kind in ('module-provider', 'module-provider-conflict'):
        old, = facts(before, kind)
        new, = facts(after, kind)
        assert any(change.get('before') == old and change.get('after') == new
                   for change in changes), (kind, changes)


def main():
    ycxx = Path(sys.argv[1]).resolve()
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-module-providers-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        scope = root / 'project'
        journal = root / 'runs'
        env = env_for(runtime)
        prepare(scope, journal)
        try:
            check_builds(ycxx, scope, env, journal)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')

        scope = root / 'provider-paths'
        try:
            check_provider_path_diff(ycxx, scope, env, journal)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
