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
from project_inspection import fact
from project_shader_bundle import check_external_actions, check_native_reused, publication
from runtime_dir import runtime_root
from shader_reflection_fixture import REJECT, digest, spirv


EXAMPLE = Path(__file__).resolve().parents[2] / 'examples/project-build/shader-reflection'
FIXTURE = Path(__file__).resolve().with_name('shader_reflection_fixture.py')
SHADERS = {'vertex': 'shaders/triangle.vert', 'fragment': 'shaders/triangle.frag'}
FRAGMENT = SHADERS['fragment']
EXPORT = 'fragment_reflection.layout'
SCHEMA = 'shader.descriptors.v1'
EXE = '.ycxx/out/app'
BUNDLE = '.ycxx/assets/app/scene.bundle'


def prepare(scope, journal):
    shutil.copytree(EXAMPLE, scope, ignore=shutil.ignore_patterns('.ycxx'))
    python = str(Path(sys.executable).resolve())
    fixture = FIXTURE.read_text(encoding='utf-8')
    manifest = scope / 'build.ycxx'
    definition = manifest.read_text(encoding='utf-8')
    for tool in ('glslc', 'spirv-cross', 'python3'):
        wrapper = scope / f'{tool}-fixture'
        command = shlex.join([python, '-c', fixture, tool, str(journal)])
        wrapper.write_text(f'#!/bin/sh\nexec {command} "$@"\n', encoding='utf-8')
        wrapper.chmod(0o700)
        default = json.dumps(f'/usr/bin/{tool}')
        assert default in definition, (default, definition)
        definition = definition.replace(default, json.dumps(str(wrapper)))
    manifest.write_text(definition, encoding='utf-8')


def layout(binding):
    return f'{SCHEMA}\nMaterial binding={binding} set=0'


def inspect(ycxx, scope, env, journal, binding=None):
    before = journal.read_bytes() if journal.exists() else None
    result = run_client(ycxx, scope, env, 'show', 'app', '--format=json')
    assert result.returncode == 0, result.stderr
    assert (journal.read_bytes() if journal.exists() else None) == before, 'show ran a shader tool'
    plan = json.loads(result.stdout)
    assert plan['schema'] == 1 and plan['target'] == 'app', plan
    if binding is None:
        assert not any(row['kind'] in {'value-export', 'fact-input'} for row in plan['facts']), plan
        assert any(row['kind'] == 'unresolved' for row in plan['facts']), plan
        return None

    export = fact(plan, 'value-export', 'fragment_reflection', layout(binding))
    consumer = fact(plan, 'fact-input', 'scene_bindings', layout(binding))
    assert export['name'] == consumer['name'] == 'layout', (export, consumer)
    assert EXPORT in consumer['origin'], consumer
    assert all(term in export['origin'] for term in (EXPORT, 'spirv-cross-fixture',
                                                    'rules/package', 'witness exit 0')), export
    assert re.search(r'artifact module receipt [0-9a-f]{32}', export['origin']), export
    fact(plan, 'schema', 'fragment_reflection', SCHEMA)
    edges = {(row['target'], row['name']): row for row in plan['facts'] if row['kind'] == 'input'}
    reflected = edges['fragment_reflection', 'module']
    packed = edges['scene', 'fragment']
    assert reflected['origin'] == packed['origin'] == 'artifact("fragment", "spirv")', edges
    assert Path(reflected['value']).parts[:2] == ('artifacts', 'module'), reflected
    assert Path(packed['value']).parts[:2] == ('artifacts', 'fragment'), packed
    assert Path(reflected['value']).name == Path(packed['value']).name, edges
    assert edges['scene', 'vertex']['origin'] == 'artifact("vertex", "spirv")', edges
    fact(plan, 'runtime-asset', 'app', BUNDLE)
    return export['origin']


def read_journal(journal):
    return [json.loads(line) for line in journal.read_text(encoding='utf-8').splitlines()]


def check_tools(records, *expected, fragment=None):
    assert Counter(row.get('stage', row['tool']) for row in records) == Counter(expected), records
    if fragment is not None:
        witness, = (row for row in records if row['tool'] == 'spirv-cross')
        assert witness['module'] == digest(fragment), witness
        assert Path(witness['path']).parts[:2] == ('artifacts', 'module'), witness


def check_bundle(scope, target='app'):
    bundle = (scope / f'.ycxx/assets/{target}/scene.bundle').read_text(encoding='ascii')
    entries = [line.split() for line in bundle.splitlines()]
    expected = {stage: spirv(stage, (scope / source).read_bytes()) for stage, source in SHADERS.items()}
    assert len(entries) == len(expected), entries
    actual = {stage: bytes.fromhex(encoded) for stage, encoded in entries}
    assert actual == expected, (actual.keys(), expected.keys())
    return actual['fragment']


def check_application(scope, binding, status=0):
    args = [str(binding)] if binding is not None else ['--runtime-only']
    result = subprocess.run([sys.executable, scope / 'run.py', *args], cwd=scope,
                            capture_output=True, timeout=30)
    assert result.returncode == status, (args, status, result.returncode, result.stdout, result.stderr)


def value_key(events, binding, outcome):
    values = [event for event in events if event.get('kind') == 'RuleValue']
    matching = {event['key'] for event in values
                if event.get('value') == layout(binding) and event['outcome'] == outcome}
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
        return repaired
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
    assert not {'fragment_reflection', 'scene_bindings'} & set(action_keys(events, 'RuleWorker')), events
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
    origin = inspect(ycxx, scope, env, journal, 1)
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
    assert inspect(ycxx, scope, env, journal, 1) != origin, 'show kept stale receipt provenance'

    repaired = check_watch(ycxx, scope, env, journal, changed, stable_key)

    # Changing the producer tool invalidates admitted provenance without running any tools.
    compiler = scope / 'glslc-fixture'
    command = compiler.read_text(encoding='utf-8')
    compiler.write_text(command + '\n# changed compiler identity\n', encoding='utf-8')
    inspect(ycxx, scope, env, journal)
    compiler.write_text(command, encoding='utf-8')
    inspect(ycxx, scope, env, journal, 3)

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
