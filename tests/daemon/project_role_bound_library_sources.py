"""Choose one generated library translation unit per consumer role (#1647)."""

from collections import Counter
import json
from pathlib import Path
import sys
import tempfile

from project_build import action_keys, check_program, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_host_cxx_libraries import COMMON, LEFT, RIGHT, prepare as prepare_diamond
from project_host_generated_sources import reports
from project_host_library_generated_headers import HOST_VIRTUAL
from project_inspection import (
    check_dependency_graph, check_dependency_html, check_target_dependencies_html, check_target_graph_html,
    dependency_edge, fact, manifest_location, scope_state,
)
from project_module_graph import PlanHTML
from project_native_host_tool import GENERATED_HEADER, NOTE, REFERENCE, SCANNER
from project_shader_bundle import check_external_actions
from project_targets import (
    DARWIN, LINUX, RAN, build, check_binary, executable, journal_contents, namespace, published, target_args,
)
from runtime_dir import runtime_root


HOST = ('host_impl', 'source', 'impl.cc')
TARGET = ('selected_impl', 'source', 'impl.cc')
PAIRED = (('paired_impl', 'host_source', 'host-impl.cc'), ('paired_impl', 'target_source', 'target-impl.cc'))
PLAIN = ('plain_impl', 'source', 'plain.cc')
UNRELATED = 'support/unrelated.cc'
EMIT = r'''
IFS=' ' read -r value < "$1" || exit 1
case "$value" in 17|23|29|41) ;; *) exit 1 ;; esac
/bin/cat "$2" > "$4" || exit 1
printf 'inline constexpr int private_score = %s;\n' "$value" > "$5" || exit 1
printf '%s\n' "$value" >> "$3"
'''
PAIR = r'''
printf '#include "detail.hh"\nint implementation_score() { return private_score + 16; }\n' > "$1" || exit 1
printf '#include "detail.hh"\nint implementation_score() { return private_score + 28; }\n' > "$2" || exit 1
printf 'inline constexpr int private_score = 1;\n' > "$3" || exit 1
printf 'pair\n' >> "$4"
'''


def reference(artifact):
    producer, output, _ = artifact
    return f'artifact("{producer}", "{output}")'


def binding(roles):
    return f'{{ host = {reference(roles[0])}, target = {reference(roles[1])} }}'


def prepare(scope, journal, generated, host, *, traversal='both', paired=False):
    definition = prepare_diamond(scope, journal)
    roles = PAIRED if paired else (HOST, TARGET)
    common = f'sources = ["{COMMON}"]'
    assert definition.count(common) == 1, definition
    definition = definition.replace(common, f'''\
sources = ["{COMMON}", glob("extra/*.cc"), {reference(PLAIN)},
               {binding(roles)}]''')
    scanner = f'sources = ["{SCANNER}"]\n    deps = ["left", "right"]'
    assert definition.count(scanner) == 1, definition
    definition = definition.replace(scanner, f'sources = ["{SCANNER}"]\n    deps = ["left", "right", "unrelated"]')
    if traversal == 'host':
        app = 'sources = ["main.cc"]\n    deps = ["left", "right"]'
        assert definition.count(app) == 1, definition
        definition = definition.replace(app, 'sources = ["main.cc"]')
    if traversal == 'target':
        definition = definition.replace(f'    runtime_assets = [{NOTE}]\n', '')
    definition = definition.replace(f'    generated_headers = [{GENERATED_HEADER}]\n', '')
    definition += f'''\

cpp.library "unrelated" {{ sources = ["{UNRELATED}"] }}
generate "{PLAIN[0]}" {{
    tool = "/bin/cp"
    args = [input("template"), output("source")]
    inputs = {{ template = "plain.in" }}
    outputs = {{ source = "{PLAIN[2]}" }}
}}
'''
    if paired:
        definition += f'''\

generate "{roles[0][0]}" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(PAIR)}, "pair",
            output("host_source"), output("target_source"), output("detail"), {json.dumps(str(generated))}]
    outputs = {{ host_source = "{roles[0][2]}" target_source = "{roles[1][2]}" detail = "detail.hh" }}
}}
'''
    else:
        for (producer, _, filename), score in zip(roles, (17, 29)):
            (scope / f'{producer}.seed').write_text(f'{score}\n', encoding='utf-8')
            (scope / f'{producer}.in').write_text(
                '#include "detail.hh"\nint implementation_score() { return private_score; }\n', encoding='utf-8')
            definition += f'''\

generate "{producer}" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(EMIT)}, "emit-impl", input("seed"), input("template"),
            {json.dumps(str(generated))}, output("source"), output("detail")]
    inputs = {{ seed = "{producer}.seed" template = "{producer}.in" }}
    outputs = {{ source = "{filename}" detail = "detail.hh" }}
}}
'''
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    (scope / 'extra').mkdir()
    (scope / 'extra/offset.cc').write_text('int glob_offset() { return 0; }\n', encoding='utf-8')
    (scope / 'plain.in').write_text('int plain_offset() { return 0; }\n', encoding='utf-8')
    macro = {LINUX: '__x86_64__', DARWIN: '__aarch64__'}[host]
    private_path = json.dumps(str(scope / HOST_VIRTUAL / roles[0][0] / 'detail.hh'))
    guard = f'''\
#if __has_include("detail.hh")
#error a private generated directory leaked into include search
#endif
#if defined({macro}) && __has_include({private_path})
#error a host peer could open a private generated sibling
#endif
'''
    (scope / COMMON).write_text(guard + '''\
#include "score_api.hh"
int implementation_score();
int plain_offset();
int glob_offset();
int common_arch() { return compile_arch; }
int common_score() { return implementation_score() + plain_offset() + glob_offset(); }
''', encoding='utf-8')
    for path in (LEFT, RIGHT):
        source = scope / path
        source.write_text(guard + source.read_text(encoding='utf-8'), encoding='utf-8')
    scanner_path = scope / SCANNER
    source = scanner_path.read_text(encoding='utf-8')
    scanner_path.write_text(guard + 'int unrelated_score();\n' +
                            source.replace('common_score() - 3;', 'common_score() - 3 + unrelated_score();'),
                            encoding='utf-8')
    (scope / UNRELATED).write_text(guard + 'int unrelated_score() { return 0; }\n', encoding='utf-8')
    main = 'int main() { return 0; }\n' if traversal == 'host' else '''\
#include "score_api.hh"
int main() {
    if (common_arch() != compile_arch) return 2;
    return left_score() + right_score() - common_score() - 3;
}
'''
    (scope / 'main.cc').write_text(main, encoding='utf-8')
    return definition, roles


def inspect(ycxx, scope, env, selected, journals, roles, *, traversal='both', plain_host=False):
    plan, report = reports(ycxx, scope, env, selected, *journals)
    definition = plan['definition']
    edges, sources = [], []
    deps = [('left', 'common'), ('right', 'common')]
    if traversal != 'target':
        deps += [('scanner', name) for name in ('left', 'right', 'unrelated')]
        edges += [
            dependency_edge(definition, 'app', 'scan_config', 'runtime_assets', NOTE, output='note'),
            dependency_edge(definition, 'scan_config', 'scanner', 'inputs', REFERENCE,
                            binding='scanner', output='executable'),
        ]
        sources.append(('host', HOST_VIRTUAL, roles[0]))
    if traversal != 'host':
        deps += [('app', 'left'), ('app', 'right')]
        sources.append(('target', namespace(selected) / 'virtual', roles[1]))
    edges += [
        dependency_edge(definition, consumer, producer, 'deps', json.dumps(producer),
                        start=definition.index(f'"{consumer}" {{'))
        for consumer, producer in deps
    ]
    edges.append(dependency_edge(definition, 'common', PLAIN[0], 'sources', reference(PLAIN), output=PLAIN[1]))
    if plain_host:
        spelling = reference(HOST)
        edges.append(dependency_edge(definition, 'common', HOST[0], 'sources', spelling, output=HOST[1]))
        path = str(namespace(selected) / 'virtual' / HOST[0] / HOST[2])
        assert fact(plan, 'source', 'common', path)['origin'] == spelling
    for role, directory, (producer, output, filename) in sources:
        spelling = reference((producer, output, filename))
        edge = dependency_edge(definition, 'common', producer, 'sources', spelling, output=output, binding=role)
        edges.append(edge)
        path = str(directory / producer / filename)
        row = fact(plan, 'source', 'common', path)
        assert (row['name'], row['origin']) == (path, f'{role} = {spelling}'), row
        assert (row['line'], row['column']) == (edge['location']['line'], edge['location']['column']), row
        plain = fact(plan, 'source', 'common', str(directory / PLAIN[0] / PLAIN[2]))
        assert plain['origin'] == reference(PLAIN), plain
    assert fact(plan, 'source', 'common', 'extra/offset.cc')['origin'].startswith('glob(')
    check_dependency_graph(plan, edges)
    keys = [(row['kind'], row['target'], row['name']) for row in plan['facts']]
    assert len(keys) == len(set(keys)), keys
    check_dependency_html(report, plan)
    check_target_dependencies_html(report, plan)
    check_target_graph_html(report, plan)
    return plan


def compare(ycxx, scope, env, selected, journals, plan):
    baseline = scope.with_name('saved-sources.json')
    before, logged = scope_state(scope), [journal_contents(path) for path in journals]
    args = ('show', 'app', *target_args(selected), '--against', str(baseline))
    path = str(namespace(selected) / 'virtual' / TARGET[0] / TARGET[2])
    source = fact(plan, 'source', 'common', path)
    previous = {**source, 'value': 'previous-impl.cc'}
    changed = {**plan, 'facts': [previous if row is source else row for row in plan['facts']]}
    for snapshot, changes in ((plan, []), (changed, [{'change': 'changed', 'before': previous, 'after': source}])):
        baseline.write_text(json.dumps(snapshot), encoding='utf-8')
        data = run_client(ycxx, scope, env, *args, '--format=json')
        report = run_client(ycxx, scope, env, *args, '--format=html')
        assert data.returncode == report.returncode == 0, (data.stderr, report.stderr)
        delta = json.loads(data.stdout)
        assert delta['changes'] == changes and delta['dependency_changes'] == [], delta
        section = PlanHTML(report.stdout).section('changes', 'Plan changes')
        if changes:
            row, = (node for node in section.descendants if node.tag == 'li')
            assert all(term in row.visible() for term in ('common', path, previous['value'])), row.visible()
            link, = (node for node in row.descendants if node.tag == 'a')
            assert link.attrs['href'] == f'#manifest-line-{source["line"]}', link.attrs
        else:
            assert 'No changes.' in section.visible(), section.visible()
        check_target_graph_html(report.stdout, plan)
        assert scope_state(scope) == before and [journal_contents(path) for path in journals] == logged


def generated_outcomes(events, producer):
    return {event['outcome'] for event in events if event.get('kind') == 'Tu'
            and producer in Path((event.get('span') or {}).get('file', '')).parts}


def check_watch(ycxx, scope, env, host, other, journal):
    watch = Watch(ycxx, scope, env, *target_args(other))
    try:
        summary, events, watched = watch.build('start watch with both role-generated sources warm')
        assert summary['actions_executed'] == 0, summary
        assert {str(scope / path) for path in (UNRELATED, f'{HOST[0]}.seed', f'{TARGET[0]}.seed')} <= set(watched), watched
        check_external_actions(events)
        initial = published(scope, other)

        replace_source(scope, f'{TARGET[0]}.seed', '41\n')
        _, events, _ = watch.build('edit only the selected private source sibling')
        check_external_actions(events, TARGET[0])
        assert action_keys(events, 'Link') == ['app'], events
        assert 'executed' in generated_outcomes(events, TARGET[0]) and not RAN & generated_outcomes(events, HOST[0])
        assert not RAN & tu_outcomes(events, Path(UNRELATED).name), events
        target_updated = published(scope, other)
        assert target_updated[0][0] != initial[0][0] and target_updated[1] == initial[1]
        assert journal_contents(journal) == '17\n'
        check_binary(scope, other, other, 17, host)

        replace_source(scope, f'{HOST[0]}.seed', '23\n')
        _, events, _ = watch.build('edit only the host private source sibling')
        check_external_actions(events, HOST[0], 'scan_config')
        assert action_keys(events, 'Link') == ['scanner'], events
        assert 'executed' in generated_outcomes(events, HOST[0]) and not RAN & generated_outcomes(events, TARGET[0])
        assert not RAN & tu_outcomes(events, Path(UNRELATED).name), events
        latest = published(scope, other)
        assert latest[0] == target_updated[0] and latest[1][0] != target_updated[1][0]
        assert journal_contents(journal) == '17\n23\n'
        check_binary(scope, other, other, 23, host)

        replace_source(scope, f'{HOST[0]}.seed', '  23  \n')
        _, events, _ = watch.build('reuse both compiles when generated bytes stay equal')
        check_external_actions(events, HOST[0])
        assert not RAN & (generated_outcomes(events, HOST[0]) | generated_outcomes(events, TARGET[0]))
        assert not action_keys(events, 'Link') and published(scope, other) == latest
        watch.idle('watch after equal source siblings')
    finally:
        watch.close()


def check_build(ycxx, scope, env, host, other, *, paired=False, traversal='both'):
    selected = host if paired or traversal == 'target' else other
    journal, generated = journals = tuple(scope.with_name(f'{scope.name}-{name}-runs')
                                          for name in ('scanner', 'source'))
    _, roles = prepare(scope, *journals, host, traversal=traversal, paired=paired)
    cold, _ = reports(ycxx, scope, env, selected, *journals)
    check_dependency_graph(cold, [], complete=False)
    assert not (scope / '.ycxx').exists()
    if traversal != 'both':
        inactive = TARGET if traversal == 'host' else HOST
        (scope / f'{inactive[0]}.seed').write_text('an unexpected action must fail\n', encoding='utf-8')
    if not paired and traversal == 'both':
        poison = scope / HOST_VIRTUAL / HOST[0] / 'detail.hh'
        poison.parent.mkdir(parents=True)
        poison.write_text('#error physical bytes at the generated path\n', encoding='utf-8')
    _, events = build(ycxx, scope, env, selected, selected, 'compile the shared library for its reached roles')
    reached = [index for index, role in enumerate(('host', 'target')) if traversal in ('both', role)]
    names = {roles[index][0] for index in reached}
    effects = [*names, PLAIN[0]]
    if traversal != 'target':
        effects.append('scan_config')
        assert journal_contents(journal) == '17\n'
    else:
        assert not journal.exists()
    check_external_actions(events, *effects)
    expected = ['pair'] if paired else [str((17, 29)[index]) for index in reached]
    assert Counter(journal_contents(generated).splitlines()) == Counter(expected)
    if selected == host:
        check_program(scope / executable(selected), 29)
    else:
        check_binary(scope, other, other, 17, host)
    plan = inspect(ycxx, scope, env, selected, journals, roles, traversal=traversal)
    if traversal != 'both':
        watch = Watch(ycxx, scope, env, *target_args(selected))
        try:
            summary, events, watched = watch.build('watch only the reached role producer')
            assert summary['actions_executed'] == 0, summary
            check_external_actions(events)
            assert not {str(scope / f'{inactive[0]}.{suffix}') for suffix in ('seed', 'in')} & set(watched), watched
        finally:
            watch.close()
    elif not paired:
        compare(ycxx, scope, env, selected, journals, plan)
        check_watch(ycxx, scope, env, host, other, journal)


def check_inactive_duplicate(ycxx, scope, env, host, other):
    journals = tuple(scope.with_name(f'{scope.name}-{name}-runs') for name in ('scanner', 'source'))
    journal, generated = journals
    definition, roles = prepare(scope, *journals, host, traversal='target')
    choice = binding(roles)
    definition = definition.replace(choice, f'{reference(HOST)},\n               {choice}')
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    (scope / f'{HOST[0]}.in').write_text('#include "detail.hh"\nint auxiliary_score() { return private_score; }\n',
                                        encoding='utf-8')
    _, events = build(ycxx, scope, env, host, host, 'accept distinct target sources despite an inactive host duplicate')
    check_external_actions(events, HOST[0], TARGET[0], PLAIN[0])
    assert not journal.exists() and Counter(journal_contents(generated).splitlines()) == Counter(('17', '29'))
    check_program(scope / executable(host), 29)
    inspect(ycxx, scope, env, host, journals, roles, traversal='target', plain_host=True)


def check_rejection(ycxx, scope, env, host, other, *, variant):
    journals = tuple(scope.with_name(f'{scope.name}-{name}-runs') for name in ('scanner', 'source'))
    traversal = 'target' if variant == 'duplicate-target' else 'host'
    definition, roles = prepare(scope, *journals, host, traversal=traversal)
    choice = binding(roles)
    if variant == 'missing':
        definition = definition.replace(choice, f'{{ host = {reference(HOST)} }}')
        diagnostic = "sources binding requires exactly 'host' and 'target' fields"
    elif variant == 'wrong-inactive':
        definition = definition.replace(choice, f'{{ host = {reference(HOST)}, target = "not-an-artifact.cc" }}')
        diagnostic = "expected an artifact reference for 'target' in sources"
    elif variant == 'direct':
        definition = definition.replace('sources = ["main.cc"]', f'sources = ["main.cc", {choice}]')
        diagnostic = 'expected a source path, glob or artifact reference'
    elif variant == 'host-dependent':
        field = f'seed = "{HOST[0]}.seed"'
        assert definition.count(field) == 1, definition
        definition = definition.replace(field, f'{field} helper = artifact("second_tool", "executable")')
        definition += '\ncpp.host_executable "second_tool" { sources = ["second.cc"] }\n'
        (scope / 'second.cc').write_text('int main() { return 0; }\n', encoding='utf-8')
        diagnostic = 'generated sources for host executables require a producer chain without host executable inputs'
    else:
        role = HOST if variant == 'duplicate-host' else TARGET
        spelling = reference(role)
        definition = definition.replace(choice, f'{spelling},\n               {choice}')
        selected_site = definition.index(spelling, definition.index(choice))
        where = manifest_location(definition, selected_site)
        diagnostic = f'build.ycxx:{where["line"]}:{where["column"]}: duplicate source'
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    summary, events = traced_build(ycxx, scope, env, f'reject the {variant} source binding',
                                   *target_args(other), succeeds=False)
    assert any(diagnostic in message for message in summary['diagnostics']), summary
    check_external_actions(events)
    assert not any(event.get('kind') in {'Tu', 'Link'} for event in events), events
    assert not any(path.exists() for path in (*journals, scope / executable(other)))


def main():
    ycxx = Path(sys.argv[1]).resolve()
    host, other = {'linux': (LINUX, DARWIN), 'darwin': (DARWIN, LINUX)}[sys.platform]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-role-bound-sources-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        env = env_for(runtime)
        checks = (
            ('cross', check_build, {}),
            ('native', check_build, {'paired': True}),
            ('host-only', check_build, {'traversal': 'host'}),
            ('target-only', check_build, {'traversal': 'target'}),
            ('inactive-duplicate', check_inactive_duplicate, {}),
            *((variant, check_rejection, {'variant': variant}) for variant in (
                'missing', 'wrong-inactive', 'direct', 'host-dependent', 'duplicate-host', 'duplicate-target')),
        )
        for name, check, options in checks:
            scope = root / name
            try:
                check(ycxx, scope, env, host, other, **options)
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
