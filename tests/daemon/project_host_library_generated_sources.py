"""Give each generated host library source a private sibling view (#1600)."""

from collections import Counter
import json
from pathlib import Path
import sys
import tempfile

from project_build import action_keys, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_host_cxx_libraries import COMMON, LEFT, RIGHT
from project_host_generated_sources import make_generated_only, reports
from project_host_library_generated_headers import (
    COMMON_SCHEMA, HOST_VIRTUAL, ROOT_SCHEMA, UNRELATED, poison_host_namespace, prepare as prepare_diamond,
)
from project_host_planning_inputs import COPY as INVENTORY, VERIFIER, prepare as prepare_planning
from project_inspection import (
    check_dependency_graph, check_dependency_html, check_location, check_target_dependencies_html, check_target_graph_html,
    check_value_bindings_html, dependency_edge, fact,
)
from project_native_host_tool import REFERENCE, SCANNER
from project_native_host_witness import prepare as prepare_witness
from project_shader_bundle import check_external_actions
from project_staged_witness import check_observation, score_value
from project_targets import (
    DARWIN, LINUX, RAN, build, check_binary, executable, journal_contents, namespace, published, target_args,
)
from runtime_dir import runtime_root


FIRST = 'artifact("first_code", "source")'
SECOND = 'artifact("second_code", "source")'
ROOT = 'artifact("root_code", "source")'
FALLBACK = 'fallback/extra_points.hh'
PREFERRED = 'include/extra_points.hh'
SIBLING = 'private.hh'
NAMES = ('first', 'second', 'root')
EMITTER = r'''#!/bin/sh
IFS=' ' read -r score < "$3" || exit 1
printf '%s %s\n' "$1" "$score" >> "$4" || exit 1
/bin/cat "$2" > "$5" || exit 1
printf '#define PRIVATE_POINTS %s\n' "$score" > "$6"
'''


def code_definition(scope, journal, name, seed, template):
    (scope / f'{name}.seed').write_text(f'{seed}\n', encoding='utf-8')
    (scope / f'{name}.in').write_text(template, encoding='utf-8')
    return f'''\

generate "{name}_code" {{
    tool = "/bin/sh"
    args = [input("script"), "{name}", input("template"), input("seed"), {json.dumps(str(journal))},
            output("source"), output("private")]
    inputs = {{ script = "emit-code.sh" template = "{name}.in" seed = "{name}.seed" }}
    outputs = {{ source = "{name}.cc" private = "{SIBLING}" }}
}}
'''


def append_sources(definition, source, *additions):
    sources = f'sources = ["{source}"]'
    assert definition.count(sources) == 1, definition
    return definition.replace(sources, f'sources = [{json.dumps(source)}, {", ".join(additions)}]')


def prepare(scope, journal, schemas, sources, host, other, *, host_only=False):
    definition = prepare_diamond(scope, journal, schemas, host, other, host_only=host_only)
    definition = append_sources(definition, COMMON, FIRST, SECOND)
    definition = append_sources(definition, SCANNER, ROOT)
    includes = 'public_include_dirs = ["include"]'
    assert definition.count(includes) == 1, definition
    definition = definition.replace(includes, 'public_include_dirs = ["include", "fallback"]')
    (scope / 'fallback').mkdir()
    (scope / FALLBACK).write_text('inline constexpr int extra_points = 0;\n', encoding='utf-8')
    (scope / 'emit-code.sh').write_text(EMITTER, encoding='utf-8')

    native_arch = {LINUX: '__x86_64__', DARWIN: '__aarch64__'}[host]

    def private_guard(*hidden, deny_unqualified=False):
        paths = [str(scope / HOST_VIRTUAL / f'{name}_code' / SIBLING) for name in hidden]
        if deny_unqualified:
            paths.append(SIBLING)
        guards = [f'#if defined({native_arch})\n']
        for path in paths:
            guards.append(f'#if __has_include({json.dumps(path)})\n#error leaked a private source sibling\n#endif\n')
        return ''.join(guards) + '#endif\n'

    first = private_guard('second', 'root') + '''\
#include "private.hh"
#include "common_schema.hh"
#include <extra_points.hh>
int first_points() { return PRIVATE_POINTS + extra_points + common_value - common_value; }
'''
    second = private_guard('first', 'root') + '''\
#include "private.hh"
#include "common_schema_sibling.hh"
int second_points() { return PRIVATE_POINTS + common_sibling - common_sibling; }
'''
    root = private_guard('first', 'second') + '''\
#include "private.hh"
#include "root_schema.hh"
int root_points() { return PRIVATE_POINTS + root_value; }
'''
    for name, value, template in (('first', 1, first), ('second', 5, second), ('root', 0, root)):
        definition += code_definition(scope, sources, name, value, template)
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')

    guard = private_guard(*NAMES, deny_unqualified=True)
    common = scope / COMMON
    text = common.read_text(encoding='utf-8')
    assert text.count('common_value + bonus') == 1, text
    common.write_text(guard + 'int first_points();\nint second_points();\n' +
                      text.replace('common_value + bonus', 'common_value + bonus + first_points() + second_points() - 6'),
                      encoding='utf-8')
    scanner = scope / SCANNER
    text = scanner.read_text(encoding='utf-8')
    assert text.count('+ unrelated_score();') == 1, text
    scanner.write_text(private_guard('first', 'second', deny_unqualified=True) + 'int root_points();\n' +
                       text.replace('+ unrelated_score();', '+ unrelated_score() + root_points();'),
                       encoding='utf-8')
    for path in (UNRELATED, RIGHT, LEFT):
        disk = scope / path
        disk.write_text(guard + disk.read_text(encoding='utf-8'), encoding='utf-8')
    return definition


def poison_generated_sources(scope):
    poison_host_namespace(scope)
    for name in NAMES:
        directory = scope / HOST_VIRTUAL / f'{name}_code'
        directory.mkdir(parents=True, exist_ok=True)
        for path in (directory / f'{name}.cc', directory / SIBLING):
            path.write_text('#error read a physical lookalike in the private host namespace\n', encoding='utf-8')


def check_plan(ycxx, scope, env, host, other, journals, *, host_only):
    plan, report = reports(ycxx, scope, env, other, *journals)
    graph = plan['dependency_graph']
    assert graph['complete'], graph
    definition = plan['definition']
    target, = (row for row in plan['facts'] if row['kind'] == 'native-target')
    assert (target['target'], target['value']) == ('app', other), target
    assert fact(plan, 'host-target', 'scanner', host)['name'] == 'triple'
    assert fact(plan, 'include', 'scanner', str(HOST_VIRTUAL / 'root_schema'))['origin'] == ROOT_SCHEMA
    assert fact(plan, 'include', 'common', str(HOST_VIRTUAL / 'common_schema'))['origin'] == COMMON_SCHEMA
    for owner, name, reference in (('common', 'first', FIRST), ('common', 'second', SECOND), ('scanner', 'root', ROOT)):
        producer = f'{name}_code'
        edge = dependency_edge(definition, owner, producer, 'sources', reference, output='source')
        assert graph['edges'].count(edge) == 1, graph
        virtual = HOST_VIRTUAL / producer
        source = fact(plan, 'source', owner, str(virtual / f'{name}.cc'))
        assert (source['name'], source['origin']) == (source['value'], reference), source
        check_location(source, definition, reference)
        if not host_only and owner == 'common':
            selected = namespace(other) / 'virtual' / producer
            source = fact(plan, 'source', owner, str(selected / f'{name}.cc'))
            assert (source['name'], source['origin']) == (source['value'], reference), source
            check_location(source, definition, reference)
        assert not any(
            row['kind'] == 'include' and Path(row['value']).name == producer for row in plan['facts']
        ), plan
    check_dependency_html(report, plan)
    check_target_dependencies_html(report, plan)
    check_target_graph_html(report, plan)
    return plan


def check_watch(ycxx, scope, env, host, other, journals):
    journal, _, _ = journals
    installed = published(scope, other)
    original = (scope / 'first.in').read_text(encoding='utf-8')
    watch = Watch(ycxx, scope, env, *target_args(other))
    try:
        summary, events, watched = watch.build('watch the real include fallback of a generated source')
        assert summary['actions_executed'] == 0, summary
        assert {str(scope / path) for path in (FALLBACK, 'first.in', 'first.seed')} <= set(watched), watched
        check_external_actions(events)
        assert not (scope / PREFERRED).exists()

        replace_source(scope, 'first.in', '\n#error rejected generated library main\n')
        summary, events, _ = watch.build('report the virtual generated main location', succeeds=False)
        diagnostic = f'{HOST_VIRTUAL}/first_code/first.cc:2:2: rejected generated library main'
        assert any(diagnostic in item for item in summary['diagnostics']), summary
        check_external_actions(events, 'first_code')
        assert published(scope, other) == installed and journal_contents(journal) == '17\n23\n'

        replace_source(scope, 'first.in', original)
        _, events, watched = watch.build('restore the last successful generated library main')
        check_external_actions(events)
        assert str(scope / FALLBACK) in watched, watched
        assert published(scope, other) == installed and journal_contents(journal) == '17\n23\n'

        replace_source(scope, PREFERRED, 'inline constexpr int extra_points = 6;\n')
        _, events, watched = watch.build('prefer a newly created real header without changing generated inputs')
        check_external_actions(events, 'scan_config')
        assert str(scope / PREFERRED) in watched, watched
        assert 'executed' in tu_outcomes(events, 'first.cc'), events
        assert not RAN & tu_outcomes(events, 'second.cc'), events
        assert published(scope, other)[0][0] != installed[0][0]
        check_binary(scope, other, other, 29, host)
        assert journal_contents(journal) == '17\n23\n29\n'
        check_plan(ycxx, scope, env, host, other, journals, host_only=True)
        watch.idle('watch after resolving the new real include')
    finally:
        watch.close()


def check_diamond(ycxx, scope, env, host, other, *, host_only=False):
    journals = tuple(scope.with_name(f'{scope.name}-{name}-runs') for name in ('scanner', 'schema', 'source'))
    journal, _, sources = journals
    prepare(scope, *journals, host, other, host_only=host_only)
    cold, _ = reports(ycxx, scope, env, other, *journals)
    check_dependency_graph(cold, [], complete=False)
    assert not (scope / '.ycxx').exists()
    poison_generated_sources(scope)

    _, events = build(ycxx, scope, env, other, other, 'compile generated units and private siblings in a library')
    check_external_actions(events, 'first_code', 'second_code', 'root_code', 'common_schema', 'left_schema',
                           'root_schema', 'scan_config')
    units = Counter()
    for event in events:
        if event.get('kind') == 'Tu' and event['outcome'] == 'executed':
            units[Path(event['key']).name] += event.get('repeat', 1)
    assert units[SCANNER] == units['main.cc'] == units['root.cc'] == 1, units
    peers = (Path(COMMON).name, Path(LEFT).name, Path(RIGHT).name, Path(UNRELATED).name, 'first.cc', 'second.cc')
    assert all(units[source] == (1 if host_only else 2) for source in peers), units
    assert Counter(journal_contents(sources).splitlines()) == Counter(('first 1', 'second 5', 'root 0'))
    assert journal_contents(journal) == '17\n'
    check_binary(scope, other, other, 17, host)
    check_plan(ycxx, scope, env, host, other, journals, host_only=host_only)

    installed = published(scope, other)
    replace_source(scope, 'first.seed', '  1  \n')
    _, events = build(ycxx, scope, env, other, other, 'reuse every compile with equal generated source bytes')
    check_external_actions(events, 'first_code')
    assert not any(RAN & tu_outcomes(events, source) for source in (*peers, 'root.cc', SCANNER, 'main.cc')), events
    assert published(scope, other) == installed and journal_contents(journal) == '17\n'

    if host_only:
        source = (scope / 'first.in').read_text(encoding='utf-8')
        replace_source(scope, 'first.in', source + '// Changed bytes, same code.\n')
        _, events = build(ycxx, scope, env, other, other, 'reuse the staged consumer when the host image stays equal')
        check_external_actions(events, 'first_code')
        assert RAN & tu_outcomes(events, 'first.cc'), events
        assert published(scope, other) == installed and journal_contents(journal) == '17\n'

    replace_source(scope, 'first.seed', '7\n')
    _, events = build(ycxx, scope, env, other, other, 'edit only the first private sibling')
    check_external_actions(events, 'first_code', 'scan_config')
    assert 'executed' in tu_outcomes(events, 'first.cc') and 'executed' in tu_outcomes(events, 'main.cc'), events
    if host_only:
        untouched = tuple(source for source in peers if source != 'first.cc') + ('root.cc', SCANNER)
        assert not any(RAN & tu_outcomes(events, source) for source in untouched), events
    assert journal_contents(journal) == '17\n23\n'
    check_binary(scope, other, other, 23, host)
    if host_only:
        check_watch(ycxx, scope, env, host, other, journals)


def check_early(ycxx, scope, env, host, other, *, witness):
    inventory_runs = scope.with_name('inventory-runs')
    sources = scope.with_name(f'{scope.name}-source-runs')
    scanner = [('scanner', SCANNER, 17)]
    if witness:
        prepare_witness(scope)
        make_generated_only(scope, scanner)
    else:
        prepare_planning(scope, inventory_runs)
        make_generated_only(scope, scanner + [('verifier', VERIFIER, 9)])
    manifest = scope / 'build.ycxx'
    definition = manifest.read_text(encoding='utf-8')
    source = 'sources = [artifact("scanner_source", "source")]'
    assert definition.count(source) == 1, definition
    definition = definition.replace(source, f'{source} deps = ["early_lib"]')
    reference = 'artifact("early_code", "source")'
    definition += f'\ncpp.library "early_lib" {{ sources = [{reference}] }}\n'
    definition += code_definition(scope, sources, 'early', 17,
                                  '#include "private.hh"\nint early_score() { return PRIVATE_POINTS; }\n')
    manifest.write_text(definition, encoding='utf-8')
    (scope / 'emit-code.sh').write_text(EMITTER, encoding='utf-8')
    (scope / 'scanner.template').write_text('int early_score();\nint main() { return early_score(); }\n',
                                            encoding='utf-8')

    for status in (17, 23):
        if status == 23:
            replace_source(scope, 'early.seed', '23\n')
        _, events = build(ycxx, scope, env, other, other, 'compile generated host library sources for an early rule')
        assert 'executed' in tu_outcomes(events, 'early.cc'), events
        assert 'early_code' in action_keys(events, 'ExternalAction'), events
        if witness:
            check_observation(events)
        else:
            assert journal_contents(inventory_runs) == ('17\n' if status == 17 else '17\n23\n')
        expected = status if witness else {17: 7, 23: 3}[status]
        check_binary(scope, other, other, expected, host)

        snapshot, = (scope / namespace(other) / 'rules').glob('target-*.json')
        stored = json.loads(snapshot.read_text(encoding='utf-8'))
        name, evidence = ('observed', 'host_files') if witness else ('retry_policy', 'planning_host_files')
        record, = (row for row in stored['rules'] if row['target'] == name)
        assert record[evidence] == [], (name, evidence, record[evidence])

        plan, report = reports(ycxx, scope, env, other, sources, inventory_runs)
        graph = plan['dependency_graph']
        assert graph['complete'], graph
        assert fact(plan, 'source', 'early_lib', str(HOST_VIRTUAL / 'early_code/early.cc'))['origin'] == reference
        assert dependency_edge(definition, 'early_lib', 'early_code', 'sources', reference,
                               output='source') in graph['edges']
        if witness:
            assert f'witness exit {status}' in fact(plan, 'value-export', 'observed', score_value(status))['origin']
            check_value_bindings_html(report, plan)
        else:
            receipt = fact(plan, 'plan-input', 'retry_policy', str(namespace(other) / 'virtual/inventory/inventory.txt'))
            assert receipt['origin'].startswith(INVENTORY), receipt
        check_dependency_html(report, plan)
        check_target_dependencies_html(report, plan)


def check_rejection(ycxx, scope, env, host, other, *, cycle):
    journals = tuple(scope.with_name(f'{scope.name}-{name}-runs') for name in ('scanner', 'schema', 'source'))
    definition = prepare(scope, *journals, host, other, host_only=True)
    binding = 'seed = "first.seed"'
    assert definition.count(binding) == 1, definition
    if cycle:
        reference = REFERENCE
        diagnostic = "host executable generated source dependency cycle at target 'scanner'"
    else:
        reference = 'artifact("other_tool", "executable")'
        definition += '\ncpp.host_executable "other_tool" { sources = ["other.cc"] }\n'
        (scope / 'other.cc').write_text('int main() { return 0; }\n', encoding='utf-8')
        diagnostic = 'generated sources for host executables require a producer chain without host executable inputs'
    (scope / 'build.ycxx').write_text(definition.replace(binding, f'{binding} helper = {reference}'), encoding='utf-8')
    summary, events = traced_build(ycxx, scope, env, 'reject a native host library source producer cycle or dependency',
                                   *target_args(other), succeeds=False)
    assert any(diagnostic in item for item in summary['diagnostics']), summary
    check_external_actions(events)
    assert not action_keys(events, 'Link') and not any(event.get('kind') == 'Tu' for event in events), events
    assert not any(path.exists() for path in (*journals, scope / executable(other)))


def main():
    ycxx = Path(sys.argv[1]).resolve()
    host, other = {'linux': (LINUX, DARWIN), 'darwin': (DARWIN, LINUX)}[sys.platform]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-host-library-sources-') as workspace,
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
