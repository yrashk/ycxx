"""Observe a typed value through a staged artifact executable (#1515)."""

import json
from pathlib import Path
import re
import shlex
import shutil
import stat
import sys
import tempfile

from project_build import action_keys, check_program, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_inspection import (
    check_dependency_graph, check_dependency_html, check_location, check_target_dependencies_html,
    check_value_bindings_html, dependency_edge, fact, scope_state,
)
from project_shader_bundle import check_external_actions, check_native_reused, publication
from project_staged_tool import journal_bytes
from runtime_dir import runtime_root


EXAMPLE = Path(__file__).resolve().parents[2] / 'examples/project-build/staged-witness'
ANALYZER = 'analyzer.sh'
RULE = 'rules/staged_witness.cc'
UNUSED = 'unused.txt'
NATIVE = '.ycxx/out/app'
ARTIFACT = 'artifact("runner", "runner")'
HEADER = 'artifact("config", "header")'
SELECTOR = 'input("analyzer")'
TOOL = 'inputs/artifacts/analyzer/analyzer.sh'
VALUE = 'value("observed", "score")'
EXPORT = 'observed.score'
SCHEMA = 'example.score.v1'


def prepare(scope, journal):
    shutil.copytree(EXAMPLE, scope, ignore=shutil.ignore_patterns('.ycxx'))
    analyzer = scope / ANALYZER
    assert analyzer.stat().st_mode & stat.S_IXUSR, 'fixture analyzer must start executable'
    shebang, script = analyzer.read_text(encoding='utf-8').split('\n', 1)
    analyzer.write_text(f"{shebang}\nprintf '%s\\n' ran >> {shlex.quote(str(journal))}\n{script}", encoding='utf-8')

    rule = scope / RULE
    source = rule.read_text(encoding='utf-8')
    inputs = 'inputs = { script = "analyzer.sh" }'
    assert source.count(inputs) == 1, source
    rule.write_text(source.replace(inputs, 'inputs = { script = "analyzer.sh" unused = "unused.txt" }'),
                    encoding='utf-8')
    (scope / UNUSED).write_text('initial evidence\n', encoding='utf-8')


def score_value(score):
    return f'{SCHEMA}\nscore value={score}'


def inspect(ycxx, scope, env, journal, score=None, *, cold=False):
    before = scope_state(scope)
    recorded = journal_bytes(journal)
    result = run_client(ycxx, scope, env, 'show', 'app', '--format=json')
    assert result.returncode == 0, result.stderr
    plan = json.loads(result.stdout)
    assert plan['schema'] == 1 and plan['target'] == 'app', plan
    if score is None:
        assert plan['dependency_graph']['complete'] is False, plan
        assert any(row['kind'] == 'unresolved' for row in plan['facts']), plan
        assert not any(row['kind'] in {'value-export', 'fact-input'} for row in plan['facts']), plan
        if cold:
            check_dependency_graph(plan, [], complete=False)
            assert not (scope / '.ycxx').exists(), 'cold show created build state'
    else:
        definition = plan['definition']
        tool = fact(plan, 'tool', 'observed', TOOL)
        assert (tool['name'], tool['origin']) == ('tool', SELECTOR), tool
        check_location(tool, definition, 'tools.observe "observed"')
        staged = fact(plan, 'input', 'observed', TOOL.removeprefix('inputs/'))
        assert staged['name'] == 'analyzer' and staged['origin'] == ARTIFACT, staged
        fact(plan, 'schema', 'observed', SCHEMA)
        exported = fact(plan, 'value-export', 'observed', score_value(score))
        consumed = fact(plan, 'fact-input', 'config', score_value(score))
        assert exported['name'] == consumed['name'] == 'score', (exported, consumed)
        assert EXPORT in consumed['origin'], consumed
        assert re.search(r'artifact analyzer receipt [0-9a-f]{32}', exported['origin']), exported
        value_edge = {**dependency_edge(definition, 'config', 'observed', 'facts', VALUE, binding='score'),
                      'export_name': 'score'}
        check_dependency_graph(plan, [
            dependency_edge(definition, 'app', 'config', 'generated_headers', HEADER, output='header'),
            dependency_edge(definition, 'observed', 'runner', 'inputs', 'tools.observe "observed"',
                            binding='analyzer', output='runner'),
        ], value_edges=[value_edge])

        result = run_client(ycxx, scope, env, 'show', 'app', '--format=html')
        assert result.returncode == 0, result.stderr
        assert TOOL in result.stdout, 'missing staged witness tool in HTML'
        check_dependency_html(result.stdout, plan)
        check_target_dependencies_html(result.stdout, plan)
        check_value_bindings_html(result.stdout, plan)
    assert journal_bytes(journal) == recorded and scope_state(scope) == before, 'inspection changed the workspace'
    return plan


def check_observation(events, *, fails=False):
    witness = action_keys(events, 'RuleWitness', 'failed' if fails else 'executed')
    classifier = action_keys(events, 'RuleClassifier')
    assert witness == [EXPORT] and classifier == ([] if fails else [EXPORT]), (witness, classifier)


def check_observation_reuse(events):
    assert not any(event.get('kind') in {'RuleWitness', 'RuleClassifier'} for event in events), events


def value_key(events, score, outcome):
    values = [event for event in events if event.get('kind') == 'RuleValue']
    keys = {event['key'] for event in values if event.get('value') == score_value(score) and event['outcome'] == outcome}
    assert len(keys) == 1, (score, outcome, values)
    return keys.pop()


def replace_analyzer(scope, source, mode):
    pending = scope / '.ycxx' / 'watch-new-analyzer'
    pending.write_text(source, encoding='utf-8')
    pending.chmod(mode)
    pending.replace(scope / ANALYZER)


def check_watch(ycxx, scope, env, journal, initial, stable_key):
    analyzer, executable = scope / ANALYZER, scope / NATIVE
    installed = publication(executable)
    original = analyzer.read_text(encoding='utf-8')
    mode = stat.S_IMODE(analyzer.stat().st_mode)
    watch = Watch(ycxx, scope, env)
    try:
        summary, events, watched = watch.build('unchanged staged witness under watch')
        assert {str(analyzer), str(scope / UNUSED)} <= set(watched), watched
        assert summary['actions_executed'] == 0, summary
        check_external_actions(events)
        check_observation_reuse(events)
        check_native_reused(events, executable, installed)

        replace_source(scope, UNUSED, 'different evidence, same executable bytes and mode\n')
        _, events, _ = watch.build('regenerate an equal analyzer')
        check_external_actions(events, 'runner')
        check_observation_reuse(events)
        check_native_reused(events, executable, installed)
        assert journal_bytes(journal) == b'ran\n', 'equal analyzer receipt relaunched the witness'

        assert original.count("'7'") == 1, original
        spaced = original.replace("'7'", "' 7 '")
        replace_analyzer(scope, spaced, mode)
        _, events, _ = watch.build('new analyzer output with the same typed score')
        check_external_actions(events, 'runner')
        check_observation(events)
        assert value_key(events, 7, 'cutoff') == stable_key
        assert not action_keys(events, 'RuleWorker') and not action_keys(events, 'RuleValueArguments'), events
        check_native_reused(events, executable, installed)
        check_program(executable, 7)
        assert journal_bytes(journal) == b'ran\nran\n'
        equal = inspect(ycxx, scope, env, journal, 7)
        assert equal['dependency_graph'] == initial['dependency_graph']
        assert (fact(equal, 'value-export', 'observed', score_value(7))['origin']
                != fact(initial, 'value-export', 'observed', score_value(7))['origin']), 'show kept stale witness provenance'

        changed = spaced.replace("' 7 '", "' 8 '")
        replace_analyzer(scope, changed, mode)
        _, events, _ = watch.build('change the typed score')
        check_external_actions(events, 'runner', 'config')
        check_observation(events)
        assert value_key(events, 8, 'executed') == stable_key
        assert action_keys(events, 'RuleWorker') == ['config'], events
        assert 'executed' in tu_outcomes(events, 'main.cc') and action_keys(events, 'Link'), events
        check_program(executable, 8)
        published = publication(executable)
        assert published[0] != installed[0], 'typed score edit kept stale native bytes'
        assert journal_bytes(journal) == b'ran\nran\nran\n'
        latest = inspect(ycxx, scope, env, journal, 8)
        assert latest['dependency_graph'] == initial['dependency_graph']

        analyzer.chmod(mode & ~0o111)
        summary, events, watched = watch.build('reject non-executable analyzer bytes', succeeds=False)
        assert str(analyzer) in watched, watched
        assert any('not executable' in item and TOOL in item for item in summary['diagnostics']), summary
        check_external_actions(events, 'runner')
        check_observation(events, fails=True)
        check_native_reused(events, executable, published)
        assert analyzer.read_text(encoding='utf-8') == changed
        assert journal_bytes(journal) == b'ran\nran\nran\n', 'non-executable analyzer was launched'
        check_program(executable, 8)
        inspect(ycxx, scope, env, journal)

        analyzer.chmod(mode)
        summary, events, _ = watch.build('restore the executable analyzer mode')
        assert summary['actions_executed'] == 0, summary
        check_external_actions(events)
        check_observation_reuse(events)
        check_native_reused(events, executable, published)
        assert journal_bytes(journal) == b'ran\nran\nran\n'
        check_program(executable, 8)
        recovered = inspect(ycxx, scope, env, journal, 8)
        assert recovered == latest, 'recovery changed the admitted value plan'
        watch.idle('recovery with an admitted analyzer')
    finally:
        watch.close()


def check_builds(ycxx, scope, env, journal):
    inspect(ycxx, scope, env, journal, cold=True)
    assert not journal.exists(), 'cold inspection launched a witness'
    _, events = traced_build(ycxx, scope, env, 'build the staged witness and app')
    check_external_actions(events, 'runner', 'config')
    check_observation(events)
    stable_key = value_key(events, 7, 'executed')
    assert 'executed' in tu_outcomes(events, 'main.cc') and action_keys(events, 'Link'), events
    assert journal_bytes(journal) == b'ran\n'
    check_program(scope / NATIVE, 7)
    initial = inspect(ycxx, scope, env, journal, 7)
    check_watch(ycxx, scope, env, journal, initial, stable_key)


def main():
    ycxx = Path(sys.argv[1]).resolve()
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-staged-witness-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        scope, journal = root / 'project', root / 'analyzer-runs'
        env = env_for(runtime)
        prepare(scope, journal)
        try:
            check_builds(ycxx, scope, env, journal)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
