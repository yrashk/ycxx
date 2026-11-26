"""Stage an ordinary generated header before compiling a native host executable (#1579)."""

from collections import Counter
import html
import json
from pathlib import Path
import shlex
import sys
import tempfile

from project_build import action_keys, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_host_planning_inputs import COPY as INVENTORY, HEADER as POLICY_HEADER, NOTE as POLICY_NOTE
from project_host_planning_inputs import prepare as prepare_planning
from project_inspection import (
    check_dependency_graph, check_dependency_html, check_location, check_target_dependencies_html,
    dependency_edge, fact, scope_state,
)
from project_native_host_tool import GENERATED_HEADER, NOTE, REFERENCE, SCANNER, STAGED, prepare as prepare_tool
from project_native_host_witness import prepare as prepare_witness
from project_shader_bundle import check_external_actions
from project_staged_witness import check_observation
from project_targets import (
    DARWIN, LINUX, RAN, build, check_binary, check_target_trace, executable, journal_contents,
    namespace, published, target_args,
)
from runtime_dir import runtime_root


SOURCE = 'artifact("schema_source", "header")'
HOST_HEADER = 'artifact("scanner_schema", "header")'
APP_HEADER = 'artifact("app_schema", "header")'
SCHEMA = 'scanner_schema.hh'
SETTINGS = 'schema.txt'
SHARED = 'shared.cc'
BONUS = 'scanner_bonus.hh'
VIRTUAL = '.ycxx/host/virtual/scanner_schema'
SCHEMA_EMITTER = rf'''#!/bin/sh
IFS=' ' read -r score note < "$1" || exit 1
case "$BUILD_TRIPLE" in
    {LINUX}) macro=__x86_64__ ;;
    {DARWIN}) macro=__aarch64__ ;;
    *) exit 2 ;;
esac
case "$TARGET_TRIPLE" in
    {LINUX}|{DARWIN}) ;;
    *) exit 3 ;;
esac
printf '%s %s %s\n' "$BUILD_TRIPLE" "$TARGET_TRIPLE" "$score" >> "$2" || exit 1
printf '#pragma once\n#if !defined(%s)\n#error wrong architecture for host schema\n#endif\n' "$macro" > "$3" || exit 1
printf 'inline constexpr int schema_score = %s;\n' "$score" >> "$3" || exit 1
printf '%s\n' "$note" > "$4"
'''


def attach_host_header(scope, journal, definition):
    (scope / SETTINGS).write_text('17 original evidence\n', encoding='utf-8')
    (scope / 'emit-schema.sh').write_text(SCHEMA_EMITTER, encoding='utf-8')
    sources = f'sources = ["{SCANNER}"]'
    assert definition.count(sources) == 1, definition
    definition = definition.replace(sources, f'{sources}\n    generated_headers = [{HOST_HEADER}]')
    definition += f'''\

generate "schema_source" {{
    tool = "/bin/sh"
    args = [input("script"), input("settings"), {json.dumps(str(journal))}, output("header"), output("detail")]
    environment = {{ BUILD_TRIPLE = host_triple() TARGET_TRIPLE = target_triple() }}
    inputs = {{ script = "emit-schema.sh" settings = "{SETTINGS}" }}
    outputs = {{ header = "{SCHEMA}" detail = "detail.txt" }}
}}

generate "scanner_schema" {{
    tool = "/bin/cp"
    args = [input("header"), output("header")]
    inputs = {{ header = {SOURCE} }}
    outputs = {{ header = "{SCHEMA}" }}
}}
'''
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    return definition


def prepare(scope, journal, schema_runs):
    definition = attach_host_header(scope, schema_runs, prepare_tool(scope, journal))
    definition = definition.replace(f'sources = ["{SCANNER}"]', f'sources = ["{SCANNER}", "{SHARED}"]')
    definition = definition.replace('sources = ["main.cc"]', f'sources = ["main.cc", "{SHARED}"]')
    definition = definition.replace(f'generated_headers = [{GENERATED_HEADER}]',
                                    f'generated_headers = [{GENERATED_HEADER}, {APP_HEADER}]')
    definition += f'''\

generate "app_schema" {{
    tool = "/bin/cp"
    args = [input("header"), output("header")]
    inputs = {{ header = "app_schema.hh" }}
    outputs = {{ header = "{SCHEMA}" }}
}}
'''
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    (scope / 'app_schema.hh').write_text('inline constexpr int schema_score = 11;\n', encoding='utf-8')
    (scope / SHARED).write_text(f'#include "{SCHEMA}"\nint schema_value() {{ return schema_score; }}\n', encoding='utf-8')
    (scope / SCANNER).write_text(f'''\
#if __has_include("{BONUS}")
#include "{BONUS}"
#else
inline constexpr int bonus = 0;
#endif
int schema_value();
int main() {{ return schema_value() + bonus; }}
''', encoding='utf-8')
    (scope / 'main.cc').write_text(f'''\
#include "{SCHEMA}"
#include "scan_score.hh"
static_assert(schema_score == 11);
int schema_value();
int main() {{ return schema_value() == 11 ? scanned_score : 2; }}
''', encoding='utf-8')
    emitter = scope / 'emit-header.sh'
    text = emitter.read_text(encoding='utf-8')
    assert text.count('17|23)') == 1, text
    emitter.write_text(text.replace('17|23)', '17|23|29)'), encoding='utf-8')
    (scope / 'scanner_score.hh').unlink()
    return definition


def inspect(ycxx, scope, env, journal, schema_runs, other, host, *, cold=False):
    before = scope_state(scope)
    recorded = journal_contents(journal), journal_contents(schema_runs)
    args = ('show', 'app', *target_args(other))
    result = run_client(ycxx, scope, env, *args, '--format=json')
    assert result.returncode == 0, result.stderr
    plan = json.loads(result.stdout)
    assert (plan['schema'], plan['target']) == (1, 'app'), plan
    target, = (row for row in plan['facts'] if row['kind'] == 'native-target')
    assert (target['target'], target['name'], target['value']) == ('app', 'triple', other), target
    if cold:
        check_dependency_graph(plan, [], complete=False)
        assert not (scope / '.ycxx').exists(), 'cold show created build state'
    else:
        definition = plan['definition']
        host_fact = fact(plan, 'host-target', 'scanner', host)
        assert host_fact['name'] == 'triple', host_fact
        host_include = fact(plan, 'include', 'scanner', VIRTUAL)
        assert host_include['origin'] == HOST_HEADER, host_include
        check_location(host_include, definition, HOST_HEADER)
        app_include = fact(plan, 'include', 'app', str(namespace(other) / 'virtual/app_schema'))
        assert app_include['origin'] == APP_HEADER, app_include
        for variable, value, origin in (('BUILD_TRIPLE', host, 'host_triple()'),
                                        ('TARGET_TRIPLE', other, 'target_triple()')):
            setting, = (row for row in plan['facts'] if (row['kind'], row['target'], row['name']) ==
                        ('environment', 'schema_source', variable))
            assert (setting['value'], setting['origin']) == (value, origin), setting
        edges = [
            dependency_edge(definition, 'app', 'scan_config', 'generated_headers', GENERATED_HEADER, output='header'),
            dependency_edge(definition, 'app', 'app_schema', 'generated_headers', APP_HEADER, output='header'),
            dependency_edge(definition, 'app', 'scan_config', 'runtime_assets', NOTE, output='note'),
            dependency_edge(definition, 'scan_config', 'scanner', 'inputs', REFERENCE,
                            output='executable', binding='scanner'),
            dependency_edge(definition, 'scanner', 'scanner_schema', 'generated_headers', HOST_HEADER, output='header'),
            dependency_edge(definition, 'scanner_schema', 'schema_source', 'inputs', SOURCE,
                            output='header', binding='header'),
        ]
        check_dependency_graph(plan, edges)
        result = run_client(ycxx, scope, env, *args, '--format=html')
        assert result.returncode == 0, result.stderr
        visible = html.unescape(result.stdout)
        assert all(text in visible for text in (host, other, STAGED, VIRTUAL, HOST_HEADER)), visible
        check_dependency_html(result.stdout, plan)
        check_target_dependencies_html(result.stdout, plan)
    assert scope_state(scope) == before, 'show wrote to the workspace'
    assert (journal_contents(journal), journal_contents(schema_runs)) == recorded, 'show ran a project action'


def check_virtual_include(ycxx, scope, env, journal, host, other):
    directory = scope / VIRTUAL
    directory.mkdir(parents=True, exist_ok=True)
    (directory / SCHEMA).write_text('#error disk header reached the virtual header path\n', encoding='utf-8')
    shadow = 'disk_only.hh'
    (directory / shadow).write_text('#error disk fallback reached a virtual sibling\n', encoding='utf-8')
    source = (scope / SCANNER).read_text(encoding='utf-8')
    # Introduce a new probe after the disk file exists, so an old negative cache cannot satisfy it.
    guard = f'#if __has_include("{shadow}")\n#error virtual header directory fell back to disk\n#endif\n'
    replace_source(scope, SCANNER, guard + source)
    _, events = build(ycxx, scope, env, other, other, 'ignore same-named disk headers inside the virtual directory')
    check_external_actions(events)
    check_binary(scope, other, other, 17, host)
    assert journal_contents(journal) == '17\n'


def check_watch(ycxx, scope, env, journal, schema_runs, host, other):
    watch = Watch(ycxx, scope, env, *target_args(other))
    initial = published(scope, other)
    default = published(scope, None)
    schemas = journal_contents(schema_runs)
    try:
        summary, events, watched = watch.build('unchanged generated host headers under watch')
        assert summary['actions_executed'] == 0, summary
        assert {str(scope / path) for path in (SCANNER, SETTINGS)} <= set(watched), watched
        check_external_actions(events)

        replace_source(scope, SETTINGS, '23 changed header\n')
        _, events, _ = watch.build('generate a changed host schema and relink the scanner')
        check_target_trace(events, other)
        check_external_actions(events, 'schema_source', 'scanner_schema', 'scan_config')
        assert 'executed' in tu_outcomes(events, SHARED) and 'executed' in tu_outcomes(events, 'main.cc'), events
        check_binary(scope, other, other, 23, host)
        assert published(scope, None) == default and published(scope, other)[0][0] != initial[0][0]
        assert journal_contents(journal) == '17\n23\n'
        assert journal_contents(schema_runs) == schemas + f'{host} {other} 23\n'
        inspect(ycxx, scope, env, journal, schema_runs, other, host)

        assert not (scope / BONUS).exists()
        replace_source(scope, BONUS, 'inline constexpr int bonus = 6;\n')
        _, events, watched = watch.build('new real include after a recorded miss')
        assert str(scope / BONUS) in watched, watched
        check_external_actions(events, 'scan_config')
        assert 'executed' in tu_outcomes(events, SCANNER), events
        check_binary(scope, other, other, 29, host)
        assert published(scope, None) == default
        assert journal_contents(journal) == '17\n23\n29\n'
        assert journal_contents(schema_runs) == schemas + f'{host} {other} 23\n'
        watch.idle('watch after resolving the disk include')
    finally:
        watch.close()


def check_builds(ycxx, scope, env, journal, schema_runs, host, other):
    prepare(scope, journal, schema_runs)
    inspect(ycxx, scope, env, journal, schema_runs, other, host, cold=True)
    _, events = build(ycxx, scope, env, other, other, 'compile a generated header for the host during a foreign build')
    check_external_actions(events, 'schema_source', 'scanner_schema', 'scan_config', 'app_schema')
    units = Counter()
    for event in events:
        if event.get('kind') == 'Tu' and event['outcome'] == 'executed':
            units[Path(event['key']).name] += event.get('repeat', 1)
    assert units[SHARED] == 2 and units[SCANNER] == units['main.cc'] == 1, units
    check_binary(scope, other, other, 17, host)
    schemas = f'{host} {other} 17\n'
    assert journal_contents(schema_runs) == schemas and journal_contents(journal) == '17\n'
    inspect(ycxx, scope, env, journal, schema_runs, other, host)

    _, events = build(ycxx, scope, env, None, host, 'resolve generator target selector while reusing equal host headers')
    check_external_actions(events, 'schema_source')
    assert not RAN & tu_outcomes(events, SCANNER), events
    check_binary(scope, None, host, 17, host)
    schemas += f'{host} {host} 17\n'
    assert journal_contents(schema_runs) == schemas and journal_contents(journal) == '17\n'

    installed = published(scope, other)
    replace_source(scope, SETTINGS, '17 different sibling output\n')
    _, events = build(ycxx, scope, env, other, other, 'changed producer keeps selected header bytes equal')
    check_external_actions(events, 'schema_source')
    assert not RAN & (tu_outcomes(events, SCANNER) | tu_outcomes(events, SHARED) | tu_outcomes(events, 'main.cc')), events
    schemas += f'{host} {other} 17\n'
    assert journal_contents(schema_runs) == schemas and journal_contents(journal) == '17\n'
    assert published(scope, other) == installed

    check_virtual_include(ycxx, scope, env, journal, host, other)
    assert published(scope, other) == installed and journal_contents(schema_runs) == schemas
    check_watch(ycxx, scope, env, journal, schema_runs, host, other)


def check_in_flight(ycxx, scope, env, host, other):
    journal = scope.with_name('in-flight-scanner-runs')
    schema_runs = scope.with_name('in-flight-schema-runs')
    sentinel = scope.with_name('in-flight-seed-edited')
    pending = scope.with_name('in-flight-new-seed')
    seed = '17 changed during the scanner consumer\n'
    prepare(scope, journal, schema_runs)
    emitter = scope / 'emit-header.sh'
    source = emitter.read_text(encoding='utf-8')
    anchor = 'esac\n'
    assert source.count(anchor) == 1, source
    edit = f'''\
if [ ! -e {shlex.quote(str(sentinel))} ]; then
    printf '%s' {shlex.quote(seed)} > {shlex.quote(str(pending))} || exit 1
    /bin/mv -f {shlex.quote(str(pending))} {shlex.quote(str(scope / SETTINGS))} || exit 1
    : > {shlex.quote(str(sentinel))} || exit 1
fi
'''
    emitter.write_text(source.replace(anchor, anchor + edit), encoding='utf-8')

    _, events = build(ycxx, scope, env, other, other, 'keep the completed scanner effect after an equal header restage')
    assert sentinel.exists() and (scope / SETTINGS).read_text(encoding='utf-8') == seed
    assert journal_contents(schema_runs) == f'{host} {other} 17\n' * 2
    assert journal_contents(journal) == '17\n', ('scanner effect reran', journal_contents(journal))
    check_external_actions(events, 'schema_source', 'schema_source', 'scanner_schema', 'scan_config', 'app_schema')
    check_binary(scope, other, other, 17, host)
    inspect(ycxx, scope, env, journal, schema_runs, other, host)


def check_early_routes(ycxx, workspace, env, host, other):
    for mode, downstream, score in (
        ('planning', ('inventory_source', 'inventory', 'retry_policy'), 7),
        ('witness', ('config',), 17),
    ):
        scope = workspace / mode
        schema_runs = workspace / f'{mode}-schema-runs'
        if mode == 'planning':
            prepare_planning(scope, workspace / 'early-scanner-runs')
        else:
            prepare_witness(scope)
        manifest = scope / 'build.ycxx'
        attach_host_header(scope, schema_runs, manifest.read_text(encoding='utf-8'))
        (scope / SCANNER).write_text(f'#include "{SCHEMA}"\nint main() {{ return schema_score; }}\n', encoding='utf-8')
        try:
            _, events = build(ycxx, scope, env, other, other, f'stage generated host header during {mode}')
            check_external_actions(events, 'schema_source', 'scanner_schema', *downstream)
            assert 'executed' in tu_outcomes(events, SCANNER), events
            assert journal_contents(schema_runs) == f'{host} {other} 17\n'
            if mode == 'witness':
                check_observation(events)
            check_binary(scope, other, other, score, host)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


def check_dynamic_header(ycxx, scope, env, host, other):
    inventory_runs = scope.with_name('dynamic-inventory-runs')
    report_runs = scope.with_name('dynamic-report-runs')
    prepare_planning(scope, inventory_runs)
    manifest = scope / 'build.ycxx'
    definition = manifest.read_text(encoding='utf-8')
    for original, replacement in ((POLICY_HEADER, 'artifact("report_result", "header")'),
                                  (POLICY_NOTE, 'artifact("report_result", "note")')):
        assert definition.count(original) == 1, definition
        definition = definition.replace(original, replacement)
    report = r'''
"$1"
score=$?
[ "$score" -eq 7 ] || exit 1
printf '%s\n' "$score" >> "$2" || exit 1
printf 'inline constexpr int report_score = %s;\n' "$score" > "$3" || exit 1
printf '%s\n' "$score" > "$4"
'''
    reference = 'artifact("report", "executable")'
    definition += f'''\

cpp.host_executable "report" {{
    sources = ["report.cc"]
    generated_headers = [{POLICY_HEADER}]
}}

generate "report_result" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(report)}, "report", input("report"), {json.dumps(str(report_runs))},
            output("header"), output("note")]
    inputs = {{ report = {reference} }}
    outputs = {{ header = "report_score.hh" note = "score-note.txt" }}
}}
'''
    manifest.write_text(definition, encoding='utf-8')
    (scope / 'report.cc').write_text('#include "retry_limit.hh"\nint main() { return retry_limit; }\n', encoding='utf-8')
    (scope / 'main.cc').write_text('#include "report_score.hh"\nint main() { return report_score; }\n', encoding='utf-8')

    _, events = build(ycxx, scope, env, other, other, 'lower a host header using an independent native inventory tool')
    check_external_actions(events, 'inventory_source', 'inventory', 'retry_policy', 'report_result')
    assert 'retry_policy' in action_keys(events, 'RuleWorker'), events
    assert 'executed' in tu_outcomes(events, SCANNER) and 'executed' in tu_outcomes(events, 'report.cc'), events
    check_binary(scope, other, other, 7, host)
    assert journal_contents(inventory_runs) == '17\n' and journal_contents(report_runs) == '7\n'

    before = scope_state(scope)
    args = ('show', 'app', *target_args(other))
    result = run_client(ycxx, scope, env, *args, '--format=json')
    assert result.returncode == 0, result.stderr
    plan = json.loads(result.stdout)
    assert plan['dependency_graph']['complete'], plan
    fact(plan, 'host-target', 'report', host)
    fact(plan, 'host-target', 'scanner', host)
    assert fact(plan, 'include', 'report', '.ycxx/host/virtual/retry_policy')['origin'] == POLICY_HEADER
    edges = plan['dependency_graph']['edges']
    assert dependency_edge(definition, 'report_result', 'report', 'inputs', reference,
                           output='executable', binding='report') in edges
    assert dependency_edge(definition, 'report', 'retry_policy', 'generated_headers', POLICY_HEADER,
                           output='header') in edges
    assert dependency_edge(definition, 'retry_policy', 'inventory', 'plan_inputs', INVENTORY,
                           output='list', binding='inventory') in edges
    result = run_client(ycxx, scope, env, *args, '--format=html')
    assert result.returncode == 0, result.stderr
    check_target_dependencies_html(result.stdout, plan)
    assert scope_state(scope) == before
    assert journal_contents(inventory_runs) == '17\n' and journal_contents(report_runs) == '7\n'


def check_rejections(ycxx, workspace, env, other):
    for name, diagnostic in (
        ('scanner', 'host executable generated header dependency cycle'),
        ('other_tool', 'generated headers for host executables require a producer chain without host executable inputs'),
    ):
        scope = workspace / name
        journal, schema_runs = workspace / f'rejected-{name}-runs', workspace / f'rejected-{name}-schemas'
        definition = prepare(scope, journal, schema_runs)
        binding = f'settings = "{SETTINGS}"'
        assert definition.count(binding) == 1, definition
        definition = definition.replace(binding, f'{binding} executor = artifact("{name}", "executable")')
        if name == 'other_tool':
            definition += '\ncpp.host_executable "other_tool" { sources = ["other.cc"] }\n'
            (scope / 'other.cc').write_text('int main() { return 0; }\n', encoding='utf-8')
        (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
        try:
            summary, events = traced_build(ycxx, scope, env, 'reject an unsupported host schema closure',
                                           *target_args(other), succeeds=False)
            assert any(diagnostic in item for item in summary['diagnostics']), summary
            check_external_actions(events)
            assert not action_keys(events, 'Link') and not RAN & tu_outcomes(events, SCANNER), events
            assert not journal.exists() and not schema_runs.exists() and not (scope / executable(other)).exists()
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


def main():
    ycxx = Path(sys.argv[1]).resolve()
    host, other = {'linux': (LINUX, DARWIN), 'darwin': (DARWIN, LINUX)}[sys.platform]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-host-generated-headers-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        scope = root / 'project'
        env = env_for(runtime)
        try:
            check_builds(ycxx, scope, env, root / 'scanner-runs', root / 'schema-runs', host, other)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')
        in_flight = root / 'in-flight'
        try:
            check_in_flight(ycxx, in_flight, env, host, other)
        finally:
            run_client(ycxx, in_flight, env, 'ctl', 'shutdown')
        check_early_routes(ycxx, root, env, host, other)
        dynamic = root / 'dynamic'
        try:
            check_dynamic_header(ycxx, dynamic, env, host, other)
        finally:
            run_client(ycxx, dynamic, env, 'ctl', 'shutdown')
        check_rejections(ycxx, root, env, other)


if __name__ == '__main__':
    main()
