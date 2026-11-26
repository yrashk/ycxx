"""Follow native host producers through a typed witness's artifact inputs (#1571)."""

import html
import json
from pathlib import Path
import re
import shlex
import shutil
import sys
import tempfile

from project_build import action_keys, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_inspection import (
    check_dependency_graph, check_dependency_html, check_location, check_target_dependencies_html,
    check_value_bindings_html, dependency_edge, fact, scope_state,
)
from project_native_host_tool import PLANNING_UNSUPPORTED, REFERENCE, SCANNER, SCORE_HEADER, check_rejection, header
from project_native_host_witness import NOTE
from project_shader_bundle import check_external_actions
from project_staged_witness import (
    EXAMPLE, EXPORT, HEADER, RULE, SCHEMA, SELECTOR, TOOL, VALUE,
    check_observation, check_observation_reuse, score_value, value_key,
)
from project_targets import (
    DARWIN, LINUX, RAN, build, check_binary, check_target_trace, journal_contents, published, target_args,
)
from runtime_dir import runtime_root


COPY = 'artifact("analyzer", "script")'
SOURCE = 'artifact("analyzer_source", "script")'
REPAIR = 'repair_score.hh'
EMITTER = r'''#!/bin/sh
"$1"
status=$?
case "$status" in
    17|18) value=17 ;;
    19) value=' 17 ' ;;
    23) value=23 ;;
    *) printf 'unexpected scanner status %s\n' "$status" >&2; exit 1 ;;
esac
printf '%s\n' "$status" >> "$2" || exit 1
printf '#!/bin/sh\nprintf "%%s\\n" "%s"\n' "$value" > "$3" || exit 1
/bin/chmod u+x "$3" || exit 1
printf '%s\n' "$status" > "$4"
'''


def prepare(scope, journal):
    shutil.copytree(EXAMPLE, scope, ignore=shutil.ignore_patterns('.ycxx'))
    (scope / SCANNER).write_text(f'#include "{SCORE_HEADER}"\nint main() {{ return scanner_score; }}\n', encoding='utf-8')
    (scope / SCORE_HEADER).write_text(header(17), encoding='utf-8')
    (scope / 'emit-analyzer.sh').write_text(EMITTER, encoding='utf-8')

    manifest = scope / 'build.ycxx'
    definition = manifest.read_text(encoding='utf-8')
    runner = 'tools.runner "runner" {}'
    assert definition.count(runner) == 1, definition
    producers = f'''\
cpp.host_executable "scanner" {{
    sources = ["scanner.cc"]
}}

generate "analyzer_source" {{
    tool = "/bin/sh"
    args = [input("emitter"), input("scanner"), {json.dumps(str(journal))}, output("script"), output("detail")]
    inputs = {{ emitter = "emit-analyzer.sh" scanner = {REFERENCE} }}
    outputs = {{ script = "analyzer.sh" detail = "status.txt" }}
}}

generate "analyzer" {{
    tool = "/bin/cp"
    args = ["-p", input("script"), output("script")]
    inputs = {{ script = {SOURCE} }}
    outputs = {{ script = "analyzer.sh" }}
}}'''
    definition = definition.replace(runner, producers)
    headers = f'generated_headers = [{HEADER}]'
    assert definition.count(headers) == 1, definition
    manifest.write_text(definition.replace(headers, f'{headers}\n    runtime_assets = [{NOTE}]'), encoding='utf-8')

    rule = scope / RULE
    source = rule.read_text(encoding='utf-8')
    staged = 'inputs = { analyzer = artifact("runner", "runner") }'
    redirect = r' > \"$2\"",'
    assert source.count(staged) == source.count(redirect) == 1, source
    source = source.replace(staged, f'inputs = {{ analyzer = {COPY} }}')
    source = source.replace(redirect, r''' > \"$2\" || exit 1; printf '%s\\n' \"$1\" > \"$3\"",''')
    source = source.replace('output("header")]', 'output("header"), output("score")]')
    source = source.replace('header = "generated_config.hh"', 'header = "generated_config.hh" score = "score-note.txt"')
    rule.write_text(source, encoding='utf-8')


def inspect(ycxx, scope, env, journal, selector, host, *, score=None, cold=False):
    before = scope_state(scope)
    recorded = journal_contents(journal)
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
        assert (staged['name'], staged['origin']) == ('analyzer', COPY), staged
        host_fact = fact(plan, 'host-target', 'scanner', host)
        assert host_fact['name'] == 'triple', host_fact
        check_location(host_fact, definition, 'cpp.host_executable "scanner"')
        fact(plan, 'schema', 'observed', SCHEMA)
        exported = fact(plan, 'value-export', 'observed', score_value(score))
        consumed = fact(plan, 'fact-input', 'config', score_value(score))
        assert exported['name'] == consumed['name'] == 'score', (exported, consumed)
        assert re.search(r'artifact analyzer receipt [0-9a-f]{32}', exported['origin']), exported
        assert EXPORT in consumed['origin'], consumed
        value_edge = {**dependency_edge(definition, 'config', 'observed', 'facts', VALUE, binding='score'),
                      'export_name': 'score'}
        check_dependency_graph(plan, [
            dependency_edge(definition, 'app', 'config', 'generated_headers', HEADER, output='header'),
            dependency_edge(definition, 'app', 'config', 'runtime_assets', NOTE, output='score'),
            dependency_edge(definition, 'observed', 'analyzer', 'inputs', 'tools.observe "observed"',
                            output='script', binding='analyzer'),
            dependency_edge(definition, 'analyzer', 'analyzer_source', 'inputs', SOURCE,
                            output='script', binding='script'),
            dependency_edge(definition, 'analyzer_source', 'scanner', 'inputs', REFERENCE,
                            output='executable', binding='scanner'),
        ], value_edges=[value_edge])
        visible = html.unescape(result.stdout)
        assert all(text in visible for text in (host, canonical, TOOL, COPY, SOURCE, REFERENCE)), visible
        check_target_dependencies_html(result.stdout, plan)
        check_dependency_html(result.stdout, plan)
        check_value_bindings_html(result.stdout, plan)
    assert scope_state(scope) == before and journal_contents(journal) == recorded, 'show ran a project action'
    return plan


def check_watch(ycxx, scope, env, journal, host, other, value, graph):
    watch = Watch(ycxx, scope, env, *target_args(other))
    try:
        summary, events, watched = watch.build('unchanged transitive native witness under watch')
        assert summary['actions_executed'] == 0, summary
        assert {str(scope / SCANNER), str(scope / SCORE_HEADER)} <= set(watched), watched
        check_external_actions(events)
        check_observation_reuse(events)

        replace_source(scope, SCORE_HEADER, header(23))
        _, events, _ = watch.build('changed typed score reaches the foreign app')
        check_target_trace(events, other)
        check_external_actions(events, 'analyzer_source', 'analyzer', 'config')
        check_observation(events)
        assert value_key(events, 23, 'executed') == value
        assert 'executed' in tu_outcomes(events, 'main.cc'), events
        check_binary(scope, other, other, 23, host)
        updated = inspect(ycxx, scope, env, journal, other, host, score=23)
        assert updated['dependency_graph'] == graph
        installed, default = published(scope, other), published(scope, None)
        assert journal_contents(journal) == '17\n18\n19\n23\n'

        replace_source(scope, SCORE_HEADER, f'#include "{REPAIR}"\n')
        summary, events, watched = watch.build('host producer discovers a missing include', succeeds=False)
        assert any("host executable 'scanner'" in item and 'file not found' in item
                   for item in summary['diagnostics']), summary
        assert str(scope / REPAIR) in watched, watched
        check_external_actions(events)
        check_observation_reuse(events)
        assert published(scope, other) == installed and published(scope, None) == default
        inspect(ycxx, scope, env, journal, other, host)

        replace_source(scope, REPAIR, header(23))
        _, events, _ = watch.build('recover after creating just the missing scanner include')
        check_external_actions(events)
        check_observation_reuse(events)
        assert published(scope, other) == installed and published(scope, None) == default
        assert journal_contents(journal) == '17\n18\n19\n23\n'
        check_binary(scope, other, other, 23, host)
        inspect(ycxx, scope, env, journal, other, host, score=23)
        watch.idle('watch after repairing the producer chain')
    finally:
        watch.close()


def check_builds(ycxx, scope, env, journal, host, other):
    prepare(scope, journal)
    inspect(ycxx, scope, env, journal, other, host, cold=True)
    _, events = build(ycxx, scope, env, other, other, 'build a foreign app with the native producer chain')
    assert 'executed' in tu_outcomes(events, SCANNER), events
    check_external_actions(events, 'analyzer_source', 'analyzer', 'config')
    check_observation(events)
    value = value_key(events, 17, 'executed')
    check_binary(scope, other, other, 17, host)
    initial = inspect(ycxx, scope, env, journal, other, host, score=17)
    assert journal_contents(journal) == '17\n'

    _, events = build(ycxx, scope, env, None, host, 'reuse the native scanner and witness under the default target')
    assert not RAN & tu_outcomes(events, SCANNER), events
    check_external_actions(events)
    assert not action_keys(events, 'RuleWitness'), events
    check_binary(scope, None, host, 17, host)

    installed = published(scope, other)
    replace_source(scope, SCORE_HEADER, header(17) + '// Same native image.\n')
    inspect(ycxx, scope, env, journal, other, host)
    _, events = build(ycxx, scope, env, other, other, 'refresh source observations with equal native image')
    check_external_actions(events)
    check_observation_reuse(events)
    assert published(scope, other) == installed and journal_contents(journal) == '17\n'
    inspect(ycxx, scope, env, journal, other, host, score=17)

    replace_source(scope, SCORE_HEADER, header(18))
    inspect(ycxx, scope, env, journal, other, host)
    _, events = build(ycxx, scope, env, other, other, 'change scanner bytes but retain selected generator output')
    assert 'executed' in tu_outcomes(events, SCANNER), events
    check_external_actions(events, 'analyzer_source')
    check_observation_reuse(events)
    assert published(scope, other) == installed and journal_contents(journal) == '17\n18\n'
    inspect(ycxx, scope, env, journal, other, host, score=17)

    replace_source(scope, SCORE_HEADER, header(19))
    _, events = build(ycxx, scope, env, other, other, 'change staged witness bytes with the same canonical score')
    check_external_actions(events, 'analyzer_source', 'analyzer')
    check_observation(events)
    assert value_key(events, 17, 'cutoff') == value
    assert not RAN & tu_outcomes(events, 'main.cc'), events
    assert published(scope, other) == installed and journal_contents(journal) == '17\n18\n19\n'
    inspect(ycxx, scope, env, journal, other, host, score=17)
    check_watch(ycxx, scope, env, journal, host, other, value, initial['dependency_graph'])

    _, events = build(ycxx, scope, env, None, host, 'reuse the latest foreign witness under the default target')
    check_external_actions(events)
    assert not action_keys(events, 'RuleWitness'), events
    check_binary(scope, None, host, 23, host)
    assert journal_contents(journal) == '17\n18\n19\n23\n'


def check_edit_during_witness(ycxx, scope, env, host, other, *, fails=False):
    generator_runs = scope.with_name(f'{scope.name}-generator-runs')
    witness_runs = scope.with_name(f'{scope.name}-witness-runs')
    edited = scope.with_name(f'{scope.name}-edited')
    pending = scope.with_name(f'{scope.name}-new-header')
    prepare(scope, generator_runs)
    if fails:
        build(ycxx, scope, env, other, other, 'publish an app before the in-flight producer failure')
        check_binary(scope, other, other, 17, host)
        installed = published(scope, other)
    (scope / 'verifier.cc').write_text(f'''\
#include "{SCORE_HEADER}"
static_assert(scanner_score > 0);
int main() {{ return 9; }}
''', encoding='utf-8')
    # This hook is part of the generated analyzer. Only its first launch changes the shared header.
    hook = scope / 'witness-edit.sh'
    changed_header = header(31 if fails else 18)
    hook.write_text(f'''\
printf '%s\\n' ran >> {shlex.quote(str(witness_runs))} || exit 1
if [ ! -e {shlex.quote(str(edited))} ]; then
    printf '%s' {shlex.quote(changed_header)} > {shlex.quote(str(pending))} || exit 1
    /bin/mv -f {shlex.quote(str(pending))} {shlex.quote(str(scope / SCORE_HEADER))} || exit 1
    : > {shlex.quote(str(edited))} || exit 1
fi
''', encoding='utf-8')

    manifest = scope / 'build.ycxx'
    definition = manifest.read_text(encoding='utf-8')
    declaration = 'generate "analyzer_source" {'
    args = 'output("script"), output("detail")]'
    binding = f'scanner = {REFERENCE}'
    assert all(definition.count(text) == 1 for text in (declaration, args, binding)), definition
    definition = definition.replace(declaration, 'cpp.host_executable "verifier" { sources = ["verifier.cc"] }\n\n'
                                    + declaration)
    definition = definition.replace(args, args[:-1] + ', input("verifier"), input("hook")]')
    definition = definition.replace(binding, binding + ' verifier = artifact("verifier", "executable")'
                                    ' hook = "witness-edit.sh"')
    manifest.write_text(definition, encoding='utf-8')

    emitter = scope / 'emit-analyzer.sh'
    verify = r'''"$5"
[ "$?" -eq 9 ] || exit 1
'''
    script = EMITTER.replace('"$1"\n', verify + '"$1"\n', 1)
    chmod = '/bin/chmod u+x "$3"'
    script = script.replace(chmod, '/bin/cat "$6" >> "$3" || exit 1\n' + chmod)
    diagnostic = 'scanner rejected by emitter during witness revalidation'
    if fails:
        reject = f'''\
    31)
        printf '%s\\n' "$status" >> "$2" || exit 1
        printf '%s\\n' {shlex.quote(diagnostic)} >&2
        exit 37 ;;
'''
        script = script.replace('    23) value=23 ;;\n', '    23) value=23 ;;\n' + reject)
    emitter.write_text(script, encoding='utf-8')

    if fails:
        summary, events = traced_build(ycxx, scope, env, 'report the revalidated emitter failure',
                                       *target_args(other), succeeds=False)
        assert any('analyzer_source' in item and diagnostic in item for item in summary['diagnostics']), summary
        assert edited.exists() and (scope / SCORE_HEADER).read_text(encoding='utf-8') == changed_header
        assert journal_contents(generator_runs) == '17\n17\n31\n', journal_contents(generator_runs)
        assert journal_contents(witness_runs) == 'ran\n', journal_contents(witness_runs)
        check_external_actions(events, 'analyzer_source', 'analyzer', failed=('analyzer_source',))
        assert published(scope, other) == installed, 'the failed producer replaced the previous app'
        return

    _, events = build(ycxx, scope, env, other, other, 'revalidate equal analyzer bytes after a shared-header edit')
    assert edited.exists() and (scope / SCORE_HEADER).read_text(encoding='utf-8') == changed_header
    assert journal_contents(generator_runs) == '17\n18\n', journal_contents(generator_runs)
    assert journal_contents(witness_runs) == 'ran\n', journal_contents(witness_runs)
    assert all('executed' in tu_outcomes(events, source) for source in (SCANNER, 'verifier.cc')), events
    check_external_actions(events, 'analyzer_source', 'analyzer_source', 'analyzer', 'config')
    check_observation(events)
    value_key(events, 17, 'executed')
    check_binary(scope, other, other, 17, host)

    before = scope_state(scope)
    shown = run_client(ycxx, scope, env, 'show', 'app', *target_args(other), '--format=json')
    assert shown.returncode == 0, shown.stderr
    plan = json.loads(shown.stdout)
    assert plan['dependency_graph']['complete'], plan
    fact(plan, 'host-target', 'verifier', host)
    fact(plan, 'value-export', 'observed', score_value(17))
    assert scope_state(scope) == before
    assert journal_contents(generator_runs) == '17\n18\n' and journal_contents(witness_runs) == 'ran\n'


def check_planning_rejection(ycxx, scope, env, journal):
    prepare(scope, journal)
    manifest = scope / 'build.ycxx'
    definition = manifest.read_text(encoding='utf-8')
    sources = 'sources = ["main.cc"]'
    assert definition.count(sources) == 1, definition
    definition = definition.replace(sources, f'{sources}\n    plan_inputs = {{ script = {COPY} }}')
    manifest.write_text(definition, encoding='utf-8')
    check_rejection(ycxx, scope, env, journal, PLANNING_UNSUPPORTED)


def main():
    ycxx = Path(sys.argv[1]).resolve()
    host, other = {'linux': (LINUX, DARWIN), 'darwin': (DARWIN, LINUX)}[sys.platform]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-host-producer-witness-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        env = env_for(runtime)
        scope = root / 'project'
        try:
            check_builds(ycxx, scope, env, root / 'scanner-runs', host, other)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')
        for name, fails in (('in-flight', False), ('in-flight-failure', True)):
            in_flight = root / name
            try:
                check_edit_during_witness(ycxx, in_flight, env, host, other, fails=fails)
            finally:
                run_client(ycxx, in_flight, env, 'ctl', 'shutdown')
        planning = root / 'planning'
        try:
            check_planning_rejection(ycxx, planning, env, root / 'planning-runs')
        finally:
            run_client(ycxx, planning, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
