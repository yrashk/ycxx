"""Stage a resident native host executable in a foreign project build (#1559)."""

import html
import json
from pathlib import Path
import shlex
import sys
import tempfile

from project_build import action_keys, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_inspection import (
    check_dependency_graph, check_dependency_html, check_location, check_target_dependencies_html,
    dependency_edge, fact, scope_state,
)
from project_package_probe import prepare as prepare_probe
from project_shader_bundle import check_external_actions
from project_targets import (
    DARWIN, LINUX, RAN, build, check_binary, check_target_trace, executable, journal_contents,
    published, target_args,
)
from runtime_dir import runtime_root


SCANNER = 'scanner.cc'
SCORE_HEADER = 'scanner_score.hh'
REFERENCE = 'artifact("scanner", "executable")'
GENERATED_HEADER = 'artifact("scan_config", "header")'
NOTE = 'artifact("scan_config", "note")'
STAGED = 'inputs/artifacts/scanner/executable'
UNSUPPORTED = 'host executable artifacts are not available in rule planning inputs or witnesses'
EMITTER = r'''#!/bin/sh
"$1"
score=$?
case "$score" in
    17|23) ;;
    *) printf 'unexpected scanner status %s\n' "$score" >&2; exit 1 ;;
esac
printf '%s\n' "$score" >> "$2" || exit 1
printf '#pragma once\ninline constexpr int scanned_score = %s;\n' "$score" > "$3" || exit 1
printf '%s\n' "$score" > "$4"
'''


def header(score):
    return f'#pragma once\ninline constexpr int scanner_score = {score};\n'


def write_scanner(scope):
    (scope / SCANNER).write_text(f'#include "{SCORE_HEADER}"\nint main() {{ return scanner_score; }}\n', encoding='utf-8')
    (scope / SCORE_HEADER).write_text(header(17), encoding='utf-8')
    (scope / 'emit-header.sh').write_text(EMITTER, encoding='utf-8')


def producers(journal):
    return f'''\
cpp.host_executable "scanner" {{
    sources = ["scanner.cc"]
}}
generate "scan_config" {{
    tool = "/bin/sh"
    args = [input("script"), input("scanner"), {json.dumps(str(journal))}, output("header"), output("note")]
    inputs = {{ script = "emit-header.sh" scanner = {REFERENCE} }}
    outputs = {{ header = "scan_score.hh" note = "score-note.txt" }}
}}
'''


def prepare(scope, journal):
    scope.mkdir()
    write_scanner(scope)
    (scope / 'main.cc').write_text('#include "scan_score.hh"\nint main() { return scanned_score; }\n', encoding='utf-8')
    definition = producers(journal) + f'''\
cpp.executable "app" {{
    sources = ["main.cc"]
    generated_headers = [{GENERATED_HEADER}]
    runtime_assets = [{NOTE}]
}}
'''
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    return definition


def inspect(ycxx, scope, env, journal, selector, canonical, host, *, cold=False, direct=False):
    before = scope_state(scope)
    recorded = journal_contents(journal)
    result = run_client(ycxx, scope, env, 'show', 'app', *target_args(selector), '--format=json')
    assert result.returncode == 0, result.stderr
    plan = json.loads(result.stdout)
    assert plan['schema'] == 1 and plan['target'] == 'app', plan
    target, = (row for row in plan['facts'] if row['kind'] == 'native-target')
    assert (target['target'], target['name'], target['value']) == ('app', 'triple', canonical), target
    if cold:
        check_dependency_graph(plan, [], complete=False)
        assert not (scope / '.ycxx').exists(), 'cold inspection created build state'
    else:
        definition = plan['definition']
        host_fact = fact(plan, 'host-target', 'scanner', host)
        assert host_fact['name'] == 'triple', host_fact
        check_location(host_fact, definition, 'cpp.host_executable "scanner"')
        fact(plan, 'source', 'scanner', SCANNER)
        assert any((row['kind'], row['target'], row['name']) == ('output', 'scanner', 'executable')
                   for row in plan['facts']), plan
        consumer = 'direct' if direct else 'scan_config'
        staged = fact(plan, 'input', consumer, STAGED.removeprefix('inputs/'))
        assert (staged['name'], staged['origin']) == ('scanner', REFERENCE), staged
        invocation = fact(plan, 'tool' if direct else 'argv', consumer, STAGED)
        assert invocation['origin'] == 'input("scanner")', invocation
        includes = 'artifact("direct", "header")' if direct else GENERATED_HEADER
        edges = [
            dependency_edge(definition, 'app', consumer, 'generated_headers', includes, output='header'),
            dependency_edge(definition, consumer, 'scanner', 'inputs', REFERENCE,
                            output='executable', binding='scanner'),
        ]
        if not direct:
            edges.append(dependency_edge(definition, 'app', consumer, 'runtime_assets', NOTE, output='note'))
        check_dependency_graph(plan, edges)
        result = run_client(ycxx, scope, env, 'show', 'app', *target_args(selector), '--format=html')
        assert result.returncode == 0, result.stderr
        visible = html.unescape(result.stdout)
        assert all(text in visible for text in (host, canonical, STAGED, REFERENCE)), visible
        check_dependency_html(result.stdout, plan)
        check_target_dependencies_html(result.stdout, plan)
    assert scope_state(scope) == before and journal_contents(journal) == recorded, 'inspection ran the host tool'


def check_watch(ycxx, scope, env, journal, host, other):
    watched_inputs = {str(scope / file) for file in (SCANNER, SCORE_HEADER)}
    initial = published(scope, other)
    host_product = published(scope, None)
    watch = Watch(ycxx, scope, env, *target_args(other))
    try:
        summary, events, watched = watch.build('unchanged foreign project using a native scanner')
        assert summary['actions_executed'] == 0, summary
        assert watched_inputs <= set(watched), watched
        check_external_actions(events)
        assert published(scope, other) == initial

        replace_source(scope, SCORE_HEADER, header(23))
        _, events, watched = watch.build('edit an included scanner header')
        check_target_trace(events, other)
        assert 'executed' in tu_outcomes(events, SCANNER), events
        assert 'executed' in tu_outcomes(events, 'main.cc'), events
        assert watched_inputs <= set(watched), watched
        check_external_actions(events, 'scan_config')
        check_binary(scope, other, other, 23, host)
        assert journal_contents(journal) == '17\n23\n'
        installed = published(scope, other)
        assert installed[0][0] != initial[0][0], 'generated score kept stale application bytes'
        assert published(scope, None) == host_product, 'foreign watch replaced the default app'

        replace_source(scope, SCORE_HEADER, '#error rejected scanner header\n')
        summary, events, watched = watch.build('reject the native scanner compile', succeeds=False)
        assert str(scope / SCORE_HEADER) in watched, watched
        assert any('rejected scanner header' in message for message in summary['diagnostics']), summary
        check_external_actions(events)
        assert published(scope, other) == installed and published(scope, None) == host_product
        assert journal_contents(journal) == '17\n23\n', 'failed scanner compile ran the generator'

        replace_source(scope, SCORE_HEADER, header(23))
        _, events, _ = watch.build('restore the included header and reuse the successful scanner image')
        check_external_actions(events)
        assert published(scope, other) == installed and published(scope, None) == host_product
        assert journal_contents(journal) == '17\n23\n'
        check_binary(scope, other, other, 23, host)
        watch.idle('watch after scanner recovery')
    finally:
        watch.close()


def check_builds(ycxx, scope, env, journal, host, other):
    prepare(scope, journal)
    inspect(ycxx, scope, env, journal, other, other, host, cold=True)
    _, events = build(ycxx, scope, env, other, other, 'build a foreign app with a host scanner')
    assert 'executed' in tu_outcomes(events, SCANNER), events
    assert 'executed' in tu_outcomes(events, 'main.cc') and action_keys(events, 'Link'), events
    check_external_actions(events, 'scan_config')
    check_binary(scope, other, other, 17, host)
    assert journal_contents(journal) == '17\n', 'foreign build did not run the staged host executable'
    inspect(ycxx, scope, env, journal, other, other, host)

    for selector in (None, host):
        _, events = build(ycxx, scope, env, selector, host, 'reuse the host scanner for another app target')
        check_external_actions(events)
        assert not RAN & tu_outcomes(events, SCANNER), 'host scanner was rebuilt for another app target'
        check_binary(scope, selector, host, 17, host)
    assert journal_contents(journal) == '17\n', 'another app target reran the generator'

    before = published(scope, other)
    replace_source(scope, SCORE_HEADER, header(17) + '// The linked scanner image is unchanged.\n')
    inspect(ycxx, scope, env, journal, other, other, host)
    assert published(scope, other) == before
    _, events = build(ycxx, scope, env, other, other, 'reuse the generator when scanner bytes are equal')
    check_external_actions(events)
    assert published(scope, other) == before and journal_contents(journal) == '17\n'
    check_watch(ycxx, scope, env, journal, host, other)


def check_action_during_source_edit(ycxx, scope, env, host):
    journal = scope.with_name('edited-source-runs')
    prepare(scope, journal)
    main_source = scope / 'main.cc'
    original = main_source.read_text(encoding='utf-8')
    sentinel = scope.with_name('edited-source-once')
    comment = '// Edited during generation.'
    edit = f'''\
if [ "$score" = 23 ] && [ ! -e {shlex.quote(str(sentinel))} ]; then
    printf '%s\\n' {shlex.quote(comment)} >> {shlex.quote(str(main_source))} || exit 1
    : > {shlex.quote(str(sentinel))} || exit 1
fi
'''
    emitter = scope / 'emit-header.sh'
    anchor = 'esac\n'
    assert EMITTER.count(anchor) == 1, EMITTER
    emitter.write_text(EMITTER.replace(anchor, anchor + edit), encoding='utf-8')

    # Warm the app so its source is already tracked when the generator edits it.
    build(ycxx, scope, env, None, host, 'warm the source before its in-flight edit')
    check_binary(scope, None, host, 17, host)
    replace_source(scope, SCORE_HEADER, header(23))
    _, events = build(ycxx, scope, env, None, host, 'admit the unchanged action key after a source edit')
    assert sentinel.exists() and main_source.read_text(encoding='utf-8') == original + comment + '\n'
    assert journal_contents(journal) == '17\n23\n', ('source edit relaunched the generator', journal_contents(journal))
    check_external_actions(events, 'scan_config')
    check_binary(scope, None, host, 23, host)


def check_direct(ycxx, scope, env, host, other):
    scope.mkdir()
    write_scanner(scope)
    (scope / 'main.cc').write_text('int main() { return 0; }\n', encoding='utf-8')
    (scope / 'build.ycxx').write_text(f'''\
cpp.host_executable "scanner" {{ sources = ["scanner.cc"] }}
generate "direct" {{
    tool = input("scanner")
    inputs = {{ scanner = {REFERENCE} }}
    outputs = {{ header = "unused.hh" }}
}}
cpp.executable "app" {{ sources = ["main.cc"] generated_headers = [artifact("direct", "header")] }}
''', encoding='utf-8')
    summary, events = traced_build(ycxx, scope, env, 'dispatch the host scanner as the tool', *target_args(other),
                                   succeeds=False)
    assert summary['status'] == 17, summary
    check_external_actions(events, failed=('direct',))
    assert not (scope / executable(other)).exists()
    inspect(ycxx, scope, env, scope.parent / 'direct-runs', other, other, host, direct=True)


def check_rejection(ycxx, scope, env, journal, expected):
    summary, events = traced_build(ycxx, scope, env, 'reject unsupported host producer relationship', succeeds=False)
    assert any(expected in item for item in summary['diagnostics']), summary
    assert not RAN & tu_outcomes(events, SCANNER), 'invalid relationship compiled its host producer'
    assert not action_keys(events, 'RuleWitness'), events
    check_external_actions(events)
    assert not journal.exists() and not (scope / executable(None)).exists()


def check_rejections(ycxx, workspace, env):
    for name, original, changed, expected in (
        ('cycle', f'sources = ["{SCANNER}"]', f'sources = [{GENERATED_HEADER}]',
         'host executable sources require disk paths or globs'),
        ('planning', 'sources = ["main.cc"]', f'sources = ["main.cc"]\n    plan_inputs = {{ score = {GENERATED_HEADER} }}',
         UNSUPPORTED),
    ):
        scope, journal = workspace / name, workspace / f'{name}-runs'
        definition = prepare(scope, journal)
        assert definition.count(original) == 1, definition
        (scope / 'build.ycxx').write_text(definition.replace(original, changed), encoding='utf-8')
        try:
            check_rejection(ycxx, scope, env, journal, expected)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')

    scope, journal = workspace / 'witness', workspace / 'witness-generator-runs'
    witness_runs = prepare_probe(scope)
    write_scanner(scope)
    manifest = scope / 'build.ycxx'
    with manifest.open('a', encoding='utf-8') as out:
        out.write('\n' + producers(journal))
    codec = scope / 'rules/codec.cc'
    original = codec.read_text(encoding='utf-8')
    assert original.count('data = "wire.cfg"') == 1, original
    codec.write_text(original.replace('data = "wire.cfg"', f'data = {GENERATED_HEADER}'), encoding='utf-8')
    try:
        check_rejection(ycxx, scope, env, journal, UNSUPPORTED)
        assert not witness_runs.exists()
    finally:
        run_client(ycxx, scope, env, 'ctl', 'shutdown')


def main():
    ycxx = Path(sys.argv[1]).resolve()
    host, other = {'linux': (LINUX, DARWIN), 'darwin': (DARWIN, LINUX)}[sys.platform]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-native-host-tool-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        env = env_for(runtime)
        scope, journal = root / 'project', root / 'scanner-runs'
        try:
            check_builds(ycxx, scope, env, journal, host, other)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')
        edited = root / 'edited-source'
        try:
            check_action_during_source_edit(ycxx, edited, env, host)
        finally:
            run_client(ycxx, edited, env, 'ctl', 'shutdown')
        direct = root / 'direct'
        try:
            check_direct(ycxx, direct, env, host, other)
        finally:
            run_client(ycxx, direct, env, 'ctl', 'shutdown')
        check_rejections(ycxx, root, env)


if __name__ == '__main__':
    main()
