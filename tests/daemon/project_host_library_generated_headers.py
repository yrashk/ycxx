"""Scope generated header directories to native host library translation units (#1588)."""

from collections import Counter
import json
from pathlib import Path
import sys
import tempfile

from project_build import action_keys, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_host_cxx_libraries import COMMON, LEFT, REPAIR, RIGHT, VALUE_HEADER, prepare as prepare_diamond
from project_host_generated_sources import reports
from project_host_planning_inputs import COPY as INVENTORY, prepare as prepare_planning
from project_inspection import (
    check_dependency_graph, check_dependency_html, check_location, check_target_dependencies_html,
    check_value_bindings_html, dependency_edge, fact,
)
from project_native_host_tool import GENERATED_HEADER, NOTE, REFERENCE, SCANNER
from project_native_host_witness import prepare as prepare_witness
from project_shader_bundle import check_external_actions
from project_staged_witness import check_observation, score_value
from project_targets import (
    DARWIN, LINUX, RAN, build, check_binary, executable, journal_contents, namespace, published, target_args,
)
from runtime_dir import runtime_root


HOST_VIRTUAL = Path('.ycxx/host/virtual')
UNRELATED = 'support/unrelated.cc'
COMMON_SCHEMA = 'artifact("common_schema", "header")'
LEFT_SCHEMA = 'artifact("left_schema", "header")'
ROOT_SCHEMA = 'artifact("root_schema", "header")'
EARLY_SCHEMA = 'artifact("early_schema", "header")'
SCHEMA_SCRIPT = rf'''#!/bin/sh
IFS=' ' read -r value < "$2" || exit 1
case "$BUILD_TRIPLE" in
    {LINUX}|{DARWIN}) ;;
    *) exit 2 ;;
esac
case "$TARGET_TRIPLE" in
    {LINUX}) selected=1 ;;
    {DARWIN}) selected=2 ;;
    *) exit 3 ;;
esac
printf '%s %s %s %s\n' "$1" "$BUILD_TRIPLE" "$TARGET_TRIPLE" "$value" >> "$3" || exit 1
printf '#pragma once\ninline constexpr int %s_value = %s;\n' "$1" "$value" > "$4" || exit 1
printf 'inline constexpr int %s_selected = %s;\n' "$1" "$selected" >> "$4" || exit 1
printf 'inline constexpr int %s_sibling = %s;\n' "$1" "$selected" > "$5"
'''


def append_headers(definition, source, reference):
    field = f'sources = ["{source}"]'
    assert definition.count(field) == 1, definition
    return definition.replace(field, f'{field}\n    generated_headers = [{reference}]')


def prepare(scope, journal, schemas, host, other, *, host_only=False):
    definition = prepare_diamond(scope, journal)
    for source, reference in ((COMMON, COMMON_SCHEMA), (LEFT, LEFT_SCHEMA), (SCANNER, ROOT_SCHEMA)):
        definition = append_headers(definition, source, reference)
    right = f'sources = ["{RIGHT}"]\n    deps = ["common"]'
    assert definition.count(right) == 1, definition
    definition = definition.replace(right, f'sources = ["{RIGHT}"]\n    deps = ["common", "unrelated"]')
    definition += f'\ncpp.library "unrelated" {{ sources = ["{UNRELATED}"] }}\n'
    if host_only:
        app = 'sources = ["main.cc"]\n    deps = ["left", "right"]'
        assert definition.count(app) == 1, definition
        definition = definition.replace(app, 'sources = ["main.cc"]')
        (scope / 'main.cc').write_text('#include "scan_score.hh"\nint main() { return scanned_score; }\n',
                                      encoding='utf-8')
    (scope / 'write-schema.sh').write_text(SCHEMA_SCRIPT, encoding='utf-8')
    for name, value in (('common', 17), ('left', 1), ('root', 0)):
        (scope / f'{name}-schema.txt').write_text(f'{value}\n', encoding='utf-8')
        definition += f'''\

generate "{name}_schema" {{
    tool = "/bin/sh"
    args = [input("script"), "{name}", input("seed"), {json.dumps(str(schemas))}, output("header"), output("sibling")]
    environment = {{ BUILD_TRIPLE = host_triple() TARGET_TRIPLE = target_triple() }}
    inputs = {{ script = "write-schema.sh" seed = "{name}-schema.txt" }}
    outputs = {{ header = "{name}_schema.hh" sibling = "{name}_schema_sibling.hh" }}
}}
'''
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')

    native_arch = {LINUX: '__x86_64__', DARWIN: '__aarch64__'}[host]
    selected = {LINUX: 1, DARWIN: 2}[other]

    def deny(*names):
        checks = [f'#if defined({native_arch})\n']
        for name in names:
            header = json.dumps(str(scope / HOST_VIRTUAL / f'{name}_schema' / f'{name}_schema.hh'))
            checks.append(f'#if __has_include({header})\n#error leaked {name} schema into this host unit\n#endif\n')
        return ''.join(checks) + '#endif\n'

    (scope / VALUE_HEADER).write_text('inline constexpr int bonus = 0;\n', encoding='utf-8')
    (scope / COMMON).write_text(deny('left', 'root') + f'''\
#include "score_api.hh"
#include "common_schema.hh"
#include "common_schema_sibling.hh"
#include "score_value.hh"
static_assert(common_selected == {selected} && common_sibling == {selected});
int common_arch() {{ return compile_arch; }}
int common_score() {{ return common_value + bonus; }}
''', encoding='utf-8')
    (scope / LEFT).write_text(deny('root') + f'''\
#include "score_api.hh"
#include "common_schema.hh"
#include "left_schema.hh"
static_assert(common_selected == {selected} && left_selected == {selected});
int left_score() {{ return common_score() + left_value; }}
''', encoding='utf-8')
    right = scope / RIGHT
    right.write_text(deny('left', 'root') + '#include "common_schema.hh"\n' + right.read_text(encoding='utf-8'),
                     encoding='utf-8')
    (scope / UNRELATED).write_text(deny('common', 'left', 'root') + 'int unrelated_score() { return 0; }\n',
                                   encoding='utf-8')
    scanner = scope / SCANNER
    source = scanner.read_text(encoding='utf-8')
    own = json.dumps(str(scope / HOST_VIRTUAL / 'root_schema' / 'root_schema.hh'))
    left = json.dumps(str(scope / HOST_VIRTUAL / 'left_schema' / 'left_schema.hh'))
    scanner.write_text(f'''\
#include {own}
#include {left}
#include "common_schema.hh"
static_assert(root_selected == {selected} && left_selected == {selected});
int unrelated_score();
''' + source.replace('common_score() - 3', 'common_score() - 3 + root_value + unrelated_score()'), encoding='utf-8')
    app = scope / 'main.cc'
    source = app.read_text(encoding='utf-8')
    app.write_text('#if __has_include("root_schema.hh")\n#error leaked scanner-only header into app\n#endif\n' + source,
                   encoding='utf-8')
    emitter = scope / 'emit-header.sh'
    source = emitter.read_text(encoding='utf-8')
    emitter.write_text(source.replace('17|23)', '17|23|29)'), encoding='utf-8')
    return definition


def poison_host_namespace(scope):
    for name in ('common', 'left', 'root'):
        header = scope / HOST_VIRTUAL / f'{name}_schema' / f'{name}_schema.hh'
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#error a physical file replaced virtual schema bytes\n', encoding='utf-8')


def check_plan(ycxx, scope, env, journal, schemas, host, other, *, host_only):
    plan, report = reports(ycxx, scope, env, other, journal, schemas)
    definition = plan['definition']
    target, = (row for row in plan['facts'] if row['kind'] == 'native-target')
    assert (target['target'], target['value']) == ('app', other), target
    assert fact(plan, 'host-target', 'scanner', host)['name'] == 'triple'
    deps = [
        ('scanner', 'left', 'cpp.host_executable'), ('scanner', 'right', 'cpp.host_executable'),
        ('left', 'common', 'cpp.library'), ('right', 'common', 'cpp.library'),
        ('right', 'unrelated', 'cpp.library'),
    ]
    if not host_only:
        deps += [('app', 'left', 'cpp.executable'), ('app', 'right', 'cpp.executable')]
    edges = [
        dependency_edge(definition, 'app', 'scan_config', 'generated_headers', GENERATED_HEADER, output='header'),
        dependency_edge(definition, 'app', 'scan_config', 'runtime_assets', NOTE, output='note'),
        dependency_edge(definition, 'scan_config', 'scanner', 'inputs', REFERENCE,
                        output='executable', binding='scanner'),
        *(dependency_edge(definition, consumer, producer, 'deps', json.dumps(producer),
                          start=definition.index(f'{kind} "{consumer}"'))
          for consumer, producer, kind in deps),
    ]
    for owner, producer, reference in (('scanner', 'root_schema', ROOT_SCHEMA),
                                       ('common', 'common_schema', COMMON_SCHEMA), ('left', 'left_schema', LEFT_SCHEMA)):
        host_include = fact(plan, 'include', owner, str(HOST_VIRTUAL / producer))
        assert host_include['origin'] == reference, host_include
        check_location(host_include, definition, reference)
        if not host_only and owner != 'scanner':
            selected_include = fact(plan, 'include', owner, str(namespace(other) / 'virtual' / producer))
            assert selected_include['origin'] == reference
            assert selected_include['name'] != host_include['name'], (selected_include, host_include)
            check_location(selected_include, definition, reference)
        edges.append(dependency_edge(definition, owner, producer, 'generated_headers', reference, output='header'))
        for name, value, origin in (('BUILD_TRIPLE', host, 'host_triple()'),
                                    ('TARGET_TRIPLE', other, 'target_triple()')):
            setting, = (row for row in plan['facts'] if (row['kind'], row['target'], row['name']) ==
                        ('environment', producer, name))
            assert (setting['value'], setting['origin']) == (value, origin), setting
    check_dependency_graph(plan, edges)
    check_dependency_html(report, plan)
    check_target_dependencies_html(report, plan)


def check_watch(ycxx, scope, env, journal, schemas, host, other):
    installed = published(scope, other)
    watch = Watch(ycxx, scope, env, *target_args(other))
    try:
        summary, events, watched = watch.build('reuse a host library when starting watch')
        assert summary['actions_executed'] == 0, summary
        assert {str(scope / path) for path in (COMMON, VALUE_HEADER, 'left-schema.txt')} <= set(watched), watched
        check_external_actions(events)

        replace_source(scope, VALUE_HEADER, '#include "repair_score.hh"\n')
        summary, events, watched = watch.build('record a real missing include from a host library', succeeds=False)
        assert any("host executable 'scanner'" in item and 'file not found' in item for item in summary['diagnostics']), summary
        assert str(scope / VALUE_HEADER) in watched, watched
        assert not (scope / REPAIR).exists()
        check_external_actions(events)
        assert published(scope, other) == installed and journal_contents(journal) == '17\n23\n'

        replace_source(scope, REPAIR, 'inline constexpr int bonus = 6;\n')
        _, events, watched = watch.build('recover the missing host library include in one edit')
        assert str(scope / REPAIR) in watched, watched
        check_external_actions(events, 'scan_config')
        assert 'executed' in tu_outcomes(events, Path(COMMON).name), events
        check_binary(scope, other, other, 29, host)
        assert journal_contents(journal) == '17\n23\n29\n'
        check_plan(ycxx, scope, env, journal, schemas, host, other, host_only=True)
        watch.idle('watch after repairing a disk include')
    finally:
        watch.close()


def check_diamond(ycxx, scope, env, host, other, *, host_only=False):
    journal = scope.with_name(f'{scope.name}-scanner-runs')
    schemas = scope.with_name(f'{scope.name}-schema-runs')
    prepare(scope, journal, schemas, host, other, host_only=host_only)
    cold, _ = reports(ycxx, scope, env, other, journal, schemas)
    check_dependency_graph(cold, [], complete=False)
    assert not (scope / '.ycxx').exists()
    poison_host_namespace(scope)

    _, events = build(ycxx, scope, env, other, other, 'compile a generated-header diamond for the host and foreign target')
    check_external_actions(events, 'common_schema', 'left_schema', 'root_schema', 'scan_config')
    units = Counter()
    for event in events:
        if event.get('kind') == 'Tu' and event['outcome'] == 'executed':
            units[Path(event['key']).name] += event.get('repeat', 1)
    assert units[SCANNER] == units['main.cc'] == 1, units
    assert all(units[Path(source).name] == (1 if host_only else 2)
               for source in (COMMON, LEFT, RIGHT, UNRELATED)), units
    check_binary(scope, other, other, 17, host)
    assert journal_contents(journal) == '17\n'
    logged = Counter(journal_contents(schemas).splitlines())
    assert logged == Counter(f'{name} {host} {other} {value}'
                             for name, value in (('common', 17), ('left', 1), ('root', 0))), logged
    check_plan(ycxx, scope, env, journal, schemas, host, other, host_only=host_only)

    installed = published(scope, other)
    replace_source(scope, 'left-schema.txt', '  1  \n')
    _, events = build(ycxx, scope, env, other, other, 'reuse native compilation with equal generated header bytes')
    check_external_actions(events, 'left_schema')
    assert not any(RAN & tu_outcomes(events, Path(source).name)
                   for source in (SCANNER, COMMON, LEFT, RIGHT, UNRELATED, 'main.cc')), events
    assert published(scope, other) == installed and journal_contents(journal) == '17\n'

    replace_source(scope, 'left-schema.txt', '7\n')
    _, events = build(ycxx, scope, env, other, other, 'change only the left library header')
    check_external_actions(events, 'left_schema', 'scan_config')
    assert 'executed' in tu_outcomes(events, Path(LEFT).name), events
    if host_only:
        assert not any(RAN & tu_outcomes(events, Path(source).name) for source in (COMMON, RIGHT, UNRELATED)), events
    assert 'executed' in tu_outcomes(events, 'main.cc'), events
    check_binary(scope, other, other, 23, host)
    assert journal_contents(journal) == '17\n23\n'
    if host_only:
        check_watch(ycxx, scope, env, journal, schemas, host, other)


def check_early(ycxx, scope, env, host, other, *, witness):
    journal = scope.with_name('inventory-runs')
    if witness:
        prepare_witness(scope)
    else:
        prepare_planning(scope, journal)
    manifest = scope / 'build.ycxx'
    definition = manifest.read_text(encoding='utf-8')
    sources = f'sources = ["{SCANNER}"]'
    assert definition.count(sources) == 1, definition
    definition = definition.replace(sources, f'{sources} deps = ["early_lib"]')
    definition += f'''\

cpp.library "early_lib" {{
    sources = ["early.cc"]
    generated_headers = [{EARLY_SCHEMA}]
}}
generate "early_schema" {{
    tool = "/bin/cp"
    args = [input("seed"), output("header")]
    inputs = {{ seed = "early.seed" }}
    outputs = {{ header = "early.hh" }}
}}
'''
    manifest.write_text(definition, encoding='utf-8')
    (scope / SCANNER).write_text('int early_score();\nint main() { return early_score(); }\n', encoding='utf-8')
    (scope / 'early.cc').write_text('#include "early.hh"\nint early_score() { return value; }\n', encoding='utf-8')
    (scope / 'early.seed').write_text('inline constexpr int value = 17;\n', encoding='utf-8')

    for status in (17, 23):
        if status == 23:
            replace_source(scope, 'early.seed', 'inline constexpr int value = 23;\n')
        _, events = build(ycxx, scope, env, other, other, 'build a host library before completing the foreign plan')
        assert 'executed' in tu_outcomes(events, 'early.cc'), events
        assert 'early_schema' in action_keys(events, 'ExternalAction'), events
        if witness:
            check_observation(events)
        else:
            assert journal_contents(journal) == ('17\n' if status == 17 else '17\n23\n')
        expected = status if witness else {17: 7, 23: 3}[status]
        check_binary(scope, other, other, expected, host)
        plan, report = reports(ycxx, scope, env, other, journal)
        graph = plan['dependency_graph']
        assert graph['complete'], graph
        assert fact(plan, 'include', 'early_lib', str(HOST_VIRTUAL / 'early_schema'))['origin'] == EARLY_SCHEMA
        assert dependency_edge(definition, 'early_lib', 'early_schema', 'generated_headers', EARLY_SCHEMA,
                               output='header') in graph['edges']
        if witness:
            assert f'witness exit {status}' in fact(plan, 'value-export', 'observed', score_value(status))['origin']
            check_value_bindings_html(report, plan)
        else:
            receipt = fact(plan, 'plan-input', 'retry_policy', str(namespace(other) / 'virtual/inventory/inventory.txt'))
            assert receipt['origin'].startswith(INVENTORY), receipt
        check_dependency_html(report, plan)
        check_target_dependencies_html(report, plan)


def check_rejection(ycxx, scope, env, host, other, *, cycle):
    journal = scope.with_name(f'{scope.name}-scanner-runs')
    schemas = scope.with_name(f'{scope.name}-schema-runs')
    definition = prepare(scope, journal, schemas, host, other, host_only=True)
    binding = 'seed = "common-schema.txt"'
    assert definition.count(binding) == 1, definition
    if cycle:
        reference = REFERENCE
        diagnostic = "host executable generated header dependency cycle at target 'scanner'"
    else:
        reference = 'artifact("other_tool", "executable")'
        definition += '\ncpp.host_executable "other_tool" { sources = ["other.cc"] }\n'
        (scope / 'other.cc').write_text('int main() { return 0; }\n', encoding='utf-8')
        diagnostic = 'generated headers for host executables require a producer chain without host executable inputs'
    (scope / 'build.ycxx').write_text(definition.replace(binding, f'{binding} helper = {reference}'), encoding='utf-8')
    summary, events = traced_build(ycxx, scope, env, 'reject a native host library header producer cycle or dependency',
                                   *target_args(other), succeeds=False)
    assert any(diagnostic in item for item in summary['diagnostics']), summary
    check_external_actions(events)
    assert not action_keys(events, 'Link') and not any(event.get('kind') == 'Tu' for event in events), events
    assert not any(path.exists() for path in (journal, schemas, scope / executable(other)))


def main():
    ycxx = Path(sys.argv[1]).resolve()
    host, other = {'linux': (LINUX, DARWIN), 'darwin': (DARWIN, LINUX)}[sys.platform]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-host-library-headers-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        env = env_for(runtime)
        checks = (
            ('diamond', check_diamond, {}),
            ('host-only', check_diamond, {'host_only': True}),
            ('witness', check_early, {'witness': True}),
            ('planning', check_early, {'witness': False}),
            ('cycle', check_rejection, {'cycle': True}),
            ('host-producer', check_rejection, {'cycle': False}),
        )
        for name, check, options in checks:
            scope = root / name
            try:
                check(ycxx, scope, env, host, other, **options)
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
