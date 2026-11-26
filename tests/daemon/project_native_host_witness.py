"""Run a native host executable as a typed witness (#1566)."""

import html
import json
from pathlib import Path
import shutil
import sys
import tempfile

from project_build import action_keys, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_inspection import (
    check_dependency_graph, check_dependency_html, check_location, check_target_dependencies_html,
    check_value_bindings_html, dependency_edge, fact, scope_state,
)
from project_native_host_tool import REFERENCE, SCANNER, SCORE_HEADER, STAGED, header
from project_shader_bundle import check_external_actions
from project_staged_witness import (
    EXAMPLE, EXPORT, HEADER, RULE, SCHEMA, VALUE, check_observation, check_observation_reuse, score_value, value_key,
)
from project_targets import DARWIN, LINUX, RAN, build, check_binary, check_target_trace, executable, published, target_args
from runtime_dir import runtime_root


PREFERRED = f'preferred/{SCORE_HEADER}'
FALLBACK = f'fallback/{SCORE_HEADER}'
REPAIR = 'preferred/repair_score.hh'
NOTE = 'artifact("config", "score")'
SELECTOR = 'input("scanner")'


def prepare(scope):
    shutil.copytree(EXAMPLE, scope, ignore=shutil.ignore_patterns('.ycxx'))
    for directory in ('preferred', 'fallback'):
        (scope / directory).mkdir()
    (scope / FALLBACK).write_text(header(17), encoding='utf-8')
    (scope / SCANNER).write_text(f'#include <{SCORE_HEADER}>\nint main() {{ return scanner_score; }}\n', encoding='utf-8')

    manifest = scope / 'build.ycxx'
    definition = manifest.read_text(encoding='utf-8')
    runner = 'tools.runner "runner" {}'
    assert definition.count(runner) == 1, definition
    scanner = '''\
cpp.host_executable "scanner" {
    sources = ["scanner.cc"]
    public_include_dirs = ["preferred", "fallback"]
}'''
    definition = definition.replace(runner, scanner)
    headers = f'generated_headers = [{HEADER}]'
    assert definition.count(headers) == 1, definition
    manifest.write_text(definition.replace(headers, f'{headers}\n    runtime_assets = [{NOTE}]'), encoding='utf-8')

    rule = scope / RULE
    source = rule.read_text(encoding='utf-8')
    old_binding = 'inputs = { analyzer = artifact("runner", "runner") }'
    old_tool = 'tool = input("analyzer")'
    assert source.count(old_binding) == source.count(old_tool) == 1, source
    source = source.replace(old_binding, f'inputs = {{ scanner = {REFERENCE} }}')
    source = source.replace(old_tool, f'tool = {SELECTOR}')
    start, end = source.index('bool space(char c)'), source.index('void append(')
    source = source[:start] + source[end:]
    start = source.index('  if (witness.exit_status != 0)')
    end = source.index('  const ycxx_rule_fact_field_v1 value', start)
    # Status 18 exercises changed witness evidence with the same typed value as status 17.
    source = source[:start] + '''\
  const auto status = witness.exit_status;
  if (status != 17 && status != 18 && status != 23) return reject(host, "unexpected scanner status");
  const uint32_t score = status == 18 ? 17 : static_cast<uint32_t>(status);

''' + source[end:]
    header_redirect = r' > \"$2\"",'
    assert source.count(header_redirect) == 1, source
    source = source.replace(header_redirect, r''' > \"$2\" || exit 1; printf '%s\\n' \"$1\" > \"$3\"",''')
    source = source.replace('output("header")]', 'output("header"), output("score")]')
    source = source.replace('header = "generated_config.hh"', 'header = "generated_config.hh" score = "score-note.txt"')
    rule.write_text(source, encoding='utf-8')


def inspect(ycxx, scope, env, selector, host, *, status=None, cold=False):
    before = scope_state(scope)
    args = ('show', 'app', *target_args(selector))
    result = run_client(ycxx, scope, env, *args, '--format=json')
    assert result.returncode == 0, result.stderr
    plan = json.loads(result.stdout)
    canonical = selector or host
    assert plan['schema'] == 1 and plan['target'] == 'app', plan
    target, = (row for row in plan['facts'] if row['kind'] == 'native-target')
    assert (target['target'], target['name'], target['value']) == ('app', 'triple', canonical), target
    result = run_client(ycxx, scope, env, *args, '--format=html')
    assert result.returncode == 0, result.stderr
    if status is None:
        assert plan['dependency_graph']['complete'] is False, plan
        assert any(row['kind'] == 'unresolved' for row in plan['facts']), plan
        assert not any(row['kind'] in {'value-export', 'fact-input'} for row in plan['facts']), plan
        if cold:
            check_dependency_graph(plan, [], complete=False)
            assert not (scope / '.ycxx').exists(), 'cold show created build state'
    else:
        definition = plan['definition']
        tool = fact(plan, 'tool', 'observed', STAGED)
        assert (tool['name'], tool['origin']) == ('tool', SELECTOR), tool
        check_location(tool, definition, 'tools.observe "observed"')
        staged = fact(plan, 'input', 'observed', STAGED.removeprefix('inputs/'))
        assert (staged['name'], staged['origin']) == ('scanner', REFERENCE), staged
        host_fact = fact(plan, 'host-target', 'scanner', host)
        assert host_fact['name'] == 'triple', host_fact
        check_location(host_fact, definition, 'cpp.host_executable "scanner"')
        fact(plan, 'schema', 'observed', SCHEMA)
        score = 17 if status == 18 else status
        exported = fact(plan, 'value-export', 'observed', score_value(score))
        consumed = fact(plan, 'fact-input', 'config', score_value(score))
        assert exported['name'] == consumed['name'] == 'score', (exported, consumed)
        assert all(term in exported['origin'] for term in (f'via {STAGED}', f'witness exit {status}')), exported
        assert EXPORT in consumed['origin'], consumed
        value_edge = {**dependency_edge(definition, 'config', 'observed', 'facts', VALUE, binding='score'),
                      'export_name': 'score'}
        check_dependency_graph(plan, [
            dependency_edge(definition, 'app', 'config', 'generated_headers', HEADER, output='header'),
            dependency_edge(definition, 'app', 'config', 'runtime_assets', NOTE, output='score'),
            dependency_edge(definition, 'observed', 'scanner', 'inputs', 'tools.observe "observed"',
                            output='executable', binding='scanner'),
        ], value_edges=[value_edge])
        visible = html.unescape(result.stdout)
        assert all(text in visible for text in (host, canonical, STAGED, REFERENCE, f'score value={score}')), visible
        check_target_dependencies_html(result.stdout, plan)
        check_dependency_html(result.stdout, plan)
        check_value_bindings_html(result.stdout, plan)
    assert scope_state(scope) == before, 'show changed build state'
    return plan


def check_watch(ycxx, scope, env, host, other):
    installed = published(scope, other)
    default = published(scope, None)
    watch = Watch(ycxx, scope, env, *target_args(other))
    try:
        summary, events, watched = watch.build('unchanged native witness under watch')
        assert summary['actions_executed'] == 0, summary
        assert {str(scope / SCANNER), str(scope / PREFERRED)} <= set(watched), watched
        check_external_actions(events)
        check_observation_reuse(events)

        replace_source(scope, PREFERRED, '#include "repair_score.hh"\n')
        summary, events, watched = watch.build('native scanner sees a new missing include', succeeds=False)
        assert any("host executable 'scanner'" in item and 'file not found' in item
                   for item in summary['diagnostics']), summary
        assert str(scope / REPAIR) in watched, watched
        check_external_actions(events)
        check_observation_reuse(events)
        assert published(scope, other) == installed and published(scope, None) == default
        inspect(ycxx, scope, env, other, host)

        replace_source(scope, REPAIR, header(23))
        _, events, _ = watch.build('repair the newly observed missing include')
        check_target_trace(events, other)
        check_external_actions(events)
        check_observation_reuse(events)
        assert published(scope, other) == installed and published(scope, None) == default
        check_binary(scope, other, other, 23, host)
        inspect(ycxx, scope, env, other, host, status=23)
        watch.idle('watch after native witness repair')
    finally:
        watch.close()


def check_builds(ycxx, scope, env, host, other):
    prepare(scope)
    inspect(ycxx, scope, env, other, host, cold=True)
    _, events = build(ycxx, scope, env, other, other, 'build a foreign app through the native witness')
    assert 'executed' in tu_outcomes(events, SCANNER), events
    check_observation(events)
    key = value_key(events, 17, 'executed')
    check_external_actions(events, 'config')
    assert 'executed' in tu_outcomes(events, 'main.cc'), events
    check_binary(scope, other, other, 17, host)
    initial = inspect(ycxx, scope, env, other, host, status=17)

    _, events = build(ycxx, scope, env, None, host, 'reuse fixed host witness under default target')
    assert not RAN & tu_outcomes(events, SCANNER), events
    assert not action_keys(events, 'RuleWitness'), events
    check_external_actions(events)
    check_binary(scope, None, host, 17, host)
    inspect(ycxx, scope, env, None, host, status=17)

    installed = published(scope, other)
    replace_source(scope, FALLBACK, header(17) + '// The native image stays equal.\n')
    inspect(ycxx, scope, env, other, host)
    _, events = build(ycxx, scope, env, other, other, 'refresh stale source evidence with equal image bytes')
    check_external_actions(events)
    check_observation_reuse(events)
    assert published(scope, other) == installed
    inspect(ycxx, scope, env, other, host, status=17)

    replace_source(scope, FALLBACK, header(18))
    _, events = build(ycxx, scope, env, other, other, 'classify changed native exit to the same score')
    check_observation(events)
    assert value_key(events, 17, 'cutoff') == key
    check_external_actions(events)
    assert not RAN & tu_outcomes(events, 'main.cc'), events
    assert published(scope, other) == installed
    inspect(ycxx, scope, env, other, host, status=18)

    assert not (scope / PREFERRED).exists()
    replace_source(scope, PREFERRED, header(23))
    inspect(ycxx, scope, env, other, host)
    assert published(scope, other) == installed, 'read-only show rebuilt for a new include candidate'
    _, events = build(ycxx, scope, env, other, other, 'prefer a newly created include candidate')
    check_observation(events)
    assert value_key(events, 23, 'executed') == key
    check_external_actions(events, 'config')
    assert 'executed' in tu_outcomes(events, 'main.cc') and action_keys(events, 'Link'), events
    check_binary(scope, other, other, 23, host)
    assert published(scope, other)[0][0] != installed[0][0], 'changed score kept stale application bytes'
    updated = inspect(ycxx, scope, env, other, host, status=23)
    assert updated['dependency_graph'] == initial['dependency_graph']
    check_watch(ycxx, scope, env, host, other)


def check_unselected_input(ycxx, scope, env):
    prepare(scope)
    rule = scope / RULE
    source = rule.read_text(encoding='utf-8')
    tool = f'tool = {SELECTOR}'
    assert source.count(tool) == 1, source
    rule.write_text(source.replace(tool, 'tool = "/bin/sh"'), encoding='utf-8')
    summary, events = traced_build(ycxx, scope, env, 'host executable input must be the selected witness tool',
                                   succeeds=False)
    assert any('host executable witness input' in item and 'must be selected' in item
               for item in summary['diagnostics']), summary
    assert not RAN & tu_outcomes(events, SCANNER), events
    check_observation_reuse(events)
    check_external_actions(events)
    assert not (scope / executable(None)).exists()


def main():
    ycxx = Path(sys.argv[1]).resolve()
    host, other = {'linux': (LINUX, DARWIN), 'darwin': (DARWIN, LINUX)}[sys.platform]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-native-host-witness-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        env = env_for(runtime)
        scope = root / 'project'
        try:
            check_builds(ycxx, scope, env, host, other)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')
        unselected = root / 'unselected'
        try:
            check_unselected_input(ycxx, unselected, env)
        finally:
            run_client(ycxx, unselected, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
