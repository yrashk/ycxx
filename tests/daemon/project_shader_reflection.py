"""Exercise artifact-backed shader reflection through the resident daemon (#1258)."""

from collections import Counter
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

from project_build import action_keys, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_inspection import (
    check_dependency_changes_html, check_dependency_graph, check_target_dependencies_html, check_value_bindings_html,
    compare_value_bindings, dependency_edge, fact, scope_state,
)
from project_shader_bundle import check_external_actions, check_native_reused, publication
from runtime_dir import runtime_root
from shader_reflection_fixture import REJECT, digest, spirv


EXAMPLE = Path(__file__).resolve().parents[2] / 'examples/project-build/shader-reflection'
FIXTURE = Path(__file__).resolve().with_name('shader_reflection_fixture.py')
SHADERS = {'vertex': 'shaders/triangle.vert', 'fragment': 'shaders/triangle.frag'}
FRAGMENT = SHADERS['fragment']
PRODUCER = 'fragment_reflection <b data-value-probe="1">&'
EXPORT = f'{PRODUCER}.layout'
SCHEMA = 'shader.descriptors.v2'
EXE = '.ycxx/out/app'
BUNDLE = '.ycxx/assets/app/scene.bundle'


def prepare(scope, journal):
    shutil.copytree(EXAMPLE, scope, ignore=shutil.ignore_patterns('.ycxx'))
    python = str(Path(sys.executable).resolve())
    fixture = FIXTURE.read_text(encoding='utf-8')
    manifest = scope / 'build.ycxx'
    definition = manifest.read_text(encoding='utf-8')
    # The rule forwards the target name, so exercise escaped names in the plan without changing the rule.
    assert definition.count('"fragment_reflection"') == 2, definition
    definition = definition.replace('"fragment_reflection"', json.dumps(PRODUCER))
    for tool in ('glslc', 'spirv-cross', 'python3'):
        wrapper = scope / f'{tool}-fixture'
        command = shlex.join([python, '-c', fixture, tool, str(journal)])
        wrapper.write_text(f'#!/bin/sh\nexec {command} "$@"\n', encoding='utf-8')
        wrapper.chmod(0o700)
        default = json.dumps(f'/usr/bin/{tool}')
        assert default in definition, (default, definition)
        definition = definition.replace(default, json.dumps(str(wrapper)))
    manifest.write_text(definition, encoding='utf-8')


def layout(binding, texture_count=2):
    return (f'{SCHEMA}\nMaterial binding={binding} element_count=1 kind=1 set=0\n'
            f'scene_textures binding=7 element_count={texture_count} kind=2 set=1')


def inspect(ycxx, scope, env, journal, binding=None, *, texture_count=2):
    before = journal.read_bytes() if journal.exists() else None
    result = run_client(ycxx, scope, env, 'show', 'app', '--format=json')
    assert result.returncode == 0, result.stderr
    assert (journal.read_bytes() if journal.exists() else None) == before, 'show ran a shader tool'
    plan = json.loads(result.stdout)
    assert plan['schema'] == 1 and plan['target'] == 'app', plan
    if binding is None:
        assert not any(row['kind'] in {'value-export', 'fact-input'} for row in plan['facts']), plan
        assert any(row['kind'] == 'unresolved' for row in plan['facts']), plan
        assert plan['dependency_graph']['complete'] is False, plan
        return plan

    export = fact(plan, 'value-export', PRODUCER, layout(binding, texture_count))
    consumer = fact(plan, 'fact-input', 'scene_bindings', layout(binding, texture_count))
    assert export['name'] == consumer['name'] == 'layout', (export, consumer)
    assert EXPORT in consumer['origin'], consumer
    assert all(term in export['origin'] for term in (EXPORT, 'spirv-cross-fixture',
                                                    'rules/package', 'witness exit 0')), export
    assert re.search(r'artifact module receipt [0-9a-f]{32}', export['origin']), export
    fact(plan, 'schema', PRODUCER, SCHEMA)
    edges = {(row['target'], row['name']): row for row in plan['facts'] if row['kind'] == 'input'}
    reflected = edges[PRODUCER, 'module']
    packed = edges['scene', 'fragment']
    assert reflected['origin'] == packed['origin'] == 'artifact("fragment", "spirv")', edges
    assert Path(reflected['value']).parts[:2] == ('artifacts', 'module'), reflected
    assert Path(packed['value']).parts[:2] == ('artifacts', 'fragment'), packed
    assert Path(reflected['value']).name == Path(packed['value']).name, edges
    assert edges['scene', 'vertex']['origin'] == 'artifact("vertex", "spirv")', edges
    fact(plan, 'runtime-asset', 'app', BUNDLE)
    definition = plan['definition']
    value_edge = {**dependency_edge(definition, 'scene_bindings', PRODUCER, 'facts',
                                    f'value({json.dumps(PRODUCER)}, "layout")', binding='layout'),
                  'export_name': 'layout'}
    check_dependency_graph(plan, [
        dependency_edge(definition, 'app', 'scene_bindings', 'generated_headers',
                        'artifact("scene_bindings", "header")', output='header'),
        dependency_edge(definition, 'app', 'scene', 'runtime_assets', 'artifact("scene", "bundle")',
                        output='bundle'),
        dependency_edge(definition, 'scene', 'vertex', 'inputs', 'shaders.pack "scene"',
                        output='spirv', binding='vertex'),
        dependency_edge(definition, 'scene', 'fragment', 'inputs', 'shaders.pack "scene"',
                        output='spirv', binding='fragment'),
        dependency_edge(definition, PRODUCER, 'fragment', 'inputs',
                        f'shaders.reflect {json.dumps(PRODUCER)}', output='spirv', binding='module'),
    ], value_edges=[value_edge])
    return plan


def check_value_binding_views(ycxx, scope, env, journal, initial, current):
    before = scope_state(scope)
    recorded = journal.read_bytes()
    graph = current['dependency_graph']
    assert graph['value_edges'] == initial['dependency_graph']['value_edges']

    def show(format_):
        result = run_client(ycxx, scope, env, 'show', 'app', f'--format={format_}')
        assert result.returncode == 0, result.stderr
        return result.stdout

    encoded = show('json')
    assert json.loads(encoded) == current and show('json') == encoded
    document = show('html')
    check_value_bindings_html(document, current)
    check_target_dependencies_html(document, current)
    check_dependency_changes_html(document, current['definition'], None, value_bindings=True)

    baseline = scope.parent / 'value-baseline.json'
    baseline.write_text(json.dumps(initial), encoding='utf-8')
    diff = compare_value_bindings(ycxx, scope, env, baseline, current, [])
    assert diff['dependency_changes'] == [], diff
    if 'module_graph' in initial and 'module_graph' in current:
        assert 'module_changes' in diff, diff
    for kind, target in (('value-export', PRODUCER), ('fact-input', 'scene_bindings')):
        change = {'change': 'changed', 'before': fact(initial, kind, target, layout(1)),
                  'after': fact(current, kind, target, layout(3, 3))}
        assert change in diff['changes'], (change, diff)

    # A schema-1 graph from before value coverage still participates in artifact comparisons.
    added, *rest = graph['edges']
    legacy_graph = {'complete': True, 'edges': rest}
    for unavailable in (legacy_graph, {**legacy_graph, 'value_edges': None}, {**graph, 'complete': False}):
        baseline.write_text(json.dumps({**current, 'dependency_graph': unavailable}), encoding='utf-8')
        diff = compare_value_bindings(ycxx, scope, env, baseline, current, None)
        if unavailable['complete']:
            assert diff['dependency_changes'] == [{'before': [], 'after': [added]}], diff
        else:
            assert 'dependency_changes' not in diff, diff

    # A saved snapshot can contain repeated occurrences and retire a binding or export independently.
    live, = graph['value_edges']
    escaped_site = {'path': '<b data-value-binding-probe="1">&.ycxx', 'line': 78, 'column': 19}
    repeated = {**live, 'location': escaped_site}
    old_binding = {**live, 'binding': 'retired-layout'}
    old_export = {**live, 'export_name': 'retired-layout'}
    old_usage = {**live, 'usage': 'capabilities'}
    saved_graph = {**graph, 'value_edges': [old_binding, live, old_export, repeated, old_usage]}
    baseline.write_text(json.dumps({**current, 'dependency_graph': saved_graph}), encoding='utf-8')
    differences = [
        {'before': [old_usage], 'after': []},
        {'before': [repeated, live], 'after': [live]},
        {'before': [old_export], 'after': []},
        {'before': [old_binding], 'after': []},
    ]
    diff = compare_value_bindings(ycxx, scope, env, baseline, current, differences)
    assert diff['changes'] == diff['dependency_changes'] == [], diff
    assert journal.read_bytes() == recorded, 'value binding inspection ran a shader tool'
    assert scope_state(scope) == before, 'value binding inspection changed the workspace'

    baseline.write_text(json.dumps(current), encoding='utf-8')
    try:
        replace_source(scope, 'build.ycxx', '# Move the authored binding location.\n' + current['definition'])
        moved = inspect(ycxx, scope, env, journal, 3, texture_count=3)
        moved_edge, = moved['dependency_graph']['value_edges']
        assert moved_edge['location']['line'] == live['location']['line'] + 1, moved_edge
        diff = compare_value_bindings(ycxx, scope, env, baseline, moved, [])
        assert diff['changes'] == diff['dependency_changes'] == [], diff
    finally:
        replace_source(scope, 'build.ycxx', current['definition'])
    assert journal.read_bytes() == recorded, 'source-only comparison ran a shader tool'


def read_journal(journal):
    return [json.loads(line) for line in journal.read_text(encoding='utf-8').splitlines()]


def check_tools(records, *expected, fragment=None):
    assert Counter(row.get('stage', row['tool']) for row in records) == Counter(expected), records
    if fragment is not None:
        witness, = (row for row in records if row['tool'] == 'spirv-cross')
        assert witness['module'] == digest(fragment), witness
        assert Path(witness['path']).parts[:3] == ('inputs', 'artifacts', 'module'), witness


def check_bundle(scope, target='app'):
    bundle = (scope / f'.ycxx/assets/{target}/scene.bundle').read_text(encoding='ascii')
    entries = [line.split() for line in bundle.splitlines()]
    expected = {stage: spirv(stage, (scope / source).read_bytes()) for stage, source in SHADERS.items()}
    assert len(entries) == len(expected), entries
    actual = {stage: bytes.fromhex(encoded) for stage, encoded in entries}
    assert actual == expected, (actual.keys(), expected.keys())
    return actual['fragment']


def check_application(scope, binding, status=0, *, texture_count=None):
    args = [str(binding)] if binding is not None else ['--runtime-only']
    if texture_count is not None:
        args.extend(('--texture-count', str(texture_count)))
    result = subprocess.run([sys.executable, scope / 'run.py', *args], cwd=scope,
                            capture_output=True, timeout=30)
    assert result.returncode == status, (args, status, result.returncode, result.stdout, result.stderr)


def value_key(events, binding, outcome, *, texture_count=2):
    values = [event for event in events if event.get('kind') == 'RuleValue']
    matching = {event['key'] for event in values
                if event.get('value') == layout(binding, texture_count) and event['outcome'] == outcome}
    assert len(matching) == 1, (binding, outcome, values)
    return matching.pop()


def check_observation(events, *, classified=True):
    assert action_keys(events, 'RuleWitness') == [EXPORT], events
    assert action_keys(events, 'RuleClassifier') == ([EXPORT] if classified else []), events


def check_generation(events):
    assert 'scene_bindings' in action_keys(events, 'RuleWorker'), events
    assert 'scene_bindings' in action_keys(events, 'ExternalAction'), events
    assert action_keys(events, 'RuleValueArguments'), events
    assert 'executed' in tu_outcomes(events, 'main.cc') and action_keys(events, 'Link'), events


def check_reuse(scope, installed, events):
    assert 'scene_bindings' not in action_keys(events, 'RuleWorker'), events
    assert not action_keys(events, 'RuleValueArguments'), events
    assert 'scene_bindings' not in action_keys(events, 'ExternalAction'), events
    check_native_reused(events, scope / EXE, installed)


def check_watch(ycxx, scope, env, journal, original, stable_key):
    watch = Watch(ycxx, scope, env)
    try:
        summary, events, watched = watch.build('watch current shader reflection')
        installed = publication(scope / EXE)
        installed_bundle = publication(scope / BUNDLE)
        assert summary['actions_executed'] == 0, summary
        paths = {*SHADERS.values(), 'glslc-fixture', 'spirv-cross-fixture'}
        assert {str(scope / path) for path in paths} <= set(watched), watched
        check_reuse(scope, installed, events)

        assert original.count('binding = 1') == 1, original
        rebound = original.replace('binding = 1', 'binding = 3')
        offset = len(read_journal(journal))
        replace_source(scope, FRAGMENT, rebound + f'\n// {REJECT}\n')
        summary, events, watched = watch.build('reflection witness killed', succeeds=False)
        assert action_keys(events, 'RuleWitness', 'failed') == [EXPORT], events
        assert not action_keys(events, 'RuleClassifier'), events
        assert any(EXPORT in diagnostic for diagnostic in summary['diagnostics']), summary
        assert str(scope / FRAGMENT) in watched, watched
        assert publication(scope / BUNDLE) == installed_bundle, 'failed witness replaced the bundle'
        check_reuse(scope, installed, events)
        check_application(scope, 1)
        check_tools(read_journal(journal)[offset:], 'fragment', 'spirv-cross')
        inspect(ycxx, scope, env, journal)

        offset = len(read_journal(journal))
        replace_source(scope, FRAGMENT, rebound)
        _, events, _ = watch.build('watch recovers with fragment binding 3')
        check_external_actions(events, 'fragment', 'scene', 'scene_bindings')
        check_observation(events)
        check_generation(events)
        assert value_key(events, 3, 'executed') == stable_key
        fragment = check_bundle(scope)
        check_tools(read_journal(journal)[offset:], 'fragment', 'spirv-cross', 'python3', fragment=fragment)
        assert (scope / EXE).read_bytes() != installed[0], 'binding edit kept stale native code'
        check_application(scope, 3)
        check_application(scope, 1, status=4)
        inspect(ycxx, scope, env, journal, 3)
        watch.idle('successful reflection and publication')

        installed = publication(scope / EXE)
        installed_bundle = publication(scope / BUNDLE)
        offset = len(read_journal(journal))
        replace_source(scope, FRAGMENT, rebound + '\n#error invalid fragment shader\n')
        summary, events, watched = watch.build('shader compilation rejected', succeeds=False)
        check_external_actions(events, failed=('fragment',))
        assert not action_keys(events, 'RuleWitness'), events
        assert any('shader rejected by fixture' in diagnostic for diagnostic in summary['diagnostics']), summary
        assert str(scope / FRAGMENT) in watched, watched
        assert publication(scope / BUNDLE) == installed_bundle, 'failed shader compile replaced the bundle'
        check_reuse(scope, installed, events)
        check_application(scope, 3)
        check_tools(read_journal(journal)[offset:], 'fragment')

        offset = len(read_journal(journal))
        repaired = rebound + '\n// corrected shader\n'
        replace_source(scope, FRAGMENT, repaired)
        _, events, _ = watch.build('watch recovers after shader compilation failure')
        check_external_actions(events, 'fragment', 'scene')
        check_observation(events, classified=False)
        assert value_key(events, 3, 'cutoff') == stable_key
        fragment = check_bundle(scope)
        check_tools(read_journal(journal)[offset:], 'fragment', 'spirv-cross', 'python3', fragment=fragment)
        assert publication(scope / BUNDLE)[0] != installed_bundle[0], 'watch kept stale bundle bytes'
        check_reuse(scope, installed, events)
        check_application(scope, 3)
        watch.idle('recovery after failed shader publication')

        before_array_edit = inspect(ycxx, scope, env, journal, 3)
        previous_bundle = (scope / BUNDLE).read_text(encoding='ascii')
        assert repaired.count('scene_textures[2]') == 1, repaired
        resized = repaired.replace('scene_textures[2]', 'scene_textures[3]')
        offset = len(read_journal(journal))
        replace_source(scope, FRAGMENT, resized)
        _, events, watched = watch.build('edit only the fragment sampler array count')
        assert str(scope / FRAGMENT) in watched, watched
        check_external_actions(events, 'fragment', 'scene', 'scene_bindings')
        check_observation(events)
        check_generation(events)
        assert value_key(events, 3, 'executed', texture_count=3) == stable_key
        fragment = check_bundle(scope)
        check_tools(read_journal(journal)[offset:], 'fragment', 'spirv-cross', 'python3', fragment=fragment)
        assert (scope / EXE).read_bytes() != installed[0], 'array count edit kept stale native code'
        assert (scope / BUNDLE).read_text(encoding='ascii') != previous_bundle, 'array count edit kept stale SPIR-V'
        check_application(scope, 3, texture_count=3)
        check_application(scope, 3, status=4, texture_count=2)
        # Both bundles have Material binding 3. Only the sampler array count can explain this mismatch.
        stale = subprocess.run([scope / EXE, '3', previous_bundle, '3'], cwd=scope, capture_output=True, timeout=30)
        assert stale.returncode == 4, (stale.returncode, stale.stdout, stale.stderr)
        after_array_edit = inspect(ycxx, scope, env, journal, 3, texture_count=3)
        assert after_array_edit['dependency_graph'] == before_array_edit['dependency_graph']
        watch.idle('successful sampler array publication')
        return resized
    finally:
        watch.close()


def check_runtime_only(ycxx, scope, env, journal, fragment):
    # These new module bytes would kill the witness if the runtime target demanded the fact.
    replace_source(scope, FRAGMENT, fragment + f'\n// {REJECT}\n')
    offset = len(read_journal(journal))
    result = run_client(ycxx, scope, env, 'build', 'runtime_only', '--trace-engine=json')
    assert result.returncode == 0, result.stderr
    events = [json.loads(line) for line in result.stderr.splitlines()]
    check_external_actions(events, 'fragment', 'scene')
    assert not any(event.get('kind') in ('RuleWitness', 'RuleClassifier', 'RuleValue') for event in events), events
    assert not {PRODUCER, 'scene_bindings'} & set(action_keys(events, 'RuleWorker')), events
    check_tools(read_journal(journal)[offset:], 'fragment', 'python3')
    check_bundle(scope, 'runtime_only')
    check_application(scope, None)


def check_builds(ycxx, scope, env, journal):
    inspect(ycxx, scope, env, journal)
    assert not (scope / '.ycxx').exists(), 'cold show created build state'

    _, events = traced_build(ycxx, scope, env, 'cold shader reflection')
    check_external_actions(events, 'fragment', 'vertex', 'scene', 'scene_bindings')
    check_observation(events)
    check_generation(events)
    stable_key = value_key(events, 1, 'executed')
    fragment = check_bundle(scope)
    runs = read_journal(journal)
    check_tools(runs, 'vertex', 'fragment', 'spirv-cross', 'python3', fragment=fragment)
    check_application(scope, 1)
    initial = inspect(ycxx, scope, env, journal, 1)
    origin = fact(initial, 'value-export', PRODUCER, layout(1))['origin']
    installed = publication(scope / EXE)
    installed_bundle = publication(scope / BUNDLE)

    summary, events = traced_build(ycxx, scope, env, 'unchanged shader reflection')
    assert summary['actions_executed'] == 0, summary
    assert read_journal(journal) == runs
    assert publication(scope / BUNDLE) == installed_bundle, 'unchanged build replaced the bundle'
    check_reuse(scope, installed, events)

    source = (scope / FRAGMENT).read_text(encoding='utf-8')
    assert source.count('0.25') == 1, source
    changed = source.replace('0.25', '0.75')
    replace_source(scope, FRAGMENT, changed)
    inspect(ycxx, scope, env, journal)

    _, events = traced_build(ycxx, scope, env, 'change fragment implementation only')
    check_external_actions(events, 'fragment', 'scene')
    check_observation(events, classified=False)
    assert value_key(events, 1, 'cutoff') == stable_key
    fragment = check_bundle(scope)
    assert publication(scope / BUNDLE)[0] != installed_bundle[0], 'shader edit kept stale bundle bytes'
    check_tools(read_journal(journal)[len(runs):], 'fragment', 'spirv-cross', 'python3', fragment=fragment)
    check_reuse(scope, installed, events)
    check_application(scope, 1)
    refreshed = inspect(ycxx, scope, env, journal, 1)
    assert fact(refreshed, 'value-export', PRODUCER, layout(1))['origin'] != origin, (
        'show kept stale receipt provenance')

    repaired = check_watch(ycxx, scope, env, journal, changed, stable_key)

    # Changing the producer tool invalidates admitted provenance without running any tools.
    compiler = scope / 'glslc-fixture'
    command = compiler.read_text(encoding='utf-8')
    compiler.write_text(command + '\n# changed compiler identity\n', encoding='utf-8')
    try:
        inspect(ycxx, scope, env, journal)
        baseline = scope.parent / 'initial-value-plan.json'
        baseline.write_text(json.dumps(initial), encoding='utf-8')
        before = scope_state(scope)
        recorded = journal.read_bytes()
        for format_ in ('json', 'html'):
            result = run_client(ycxx, scope, env, 'show', 'app', '--against', str(baseline), f'--format={format_}')
            assert result.returncode != 0, result.stdout
            assert 'cannot compare plans with unresolved rules' in result.stderr, result.stderr
        assert scope_state(scope) == before, 'unresolved comparison changed the workspace'
        assert journal.read_bytes() == recorded, 'unresolved comparison ran a shader tool'
    finally:
        compiler.write_text(command, encoding='utf-8')
    current = inspect(ycxx, scope, env, journal, 3, texture_count=3)
    check_value_binding_views(ycxx, scope, env, journal, initial, current)

    check_runtime_only(ycxx, scope, env, journal, repaired)


def main():
    ycxx = Path(sys.argv[1]).resolve()
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-shader-reflection-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        scope = Path(workspace).resolve() / 'fixture'
        journal = scope.parent / 'tool-runs'
        prepare(scope, journal)
        env = env_for(runtime)
        try:
            check_builds(ycxx, scope, env, journal)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
