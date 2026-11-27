"""Resolve generated library header directories by host and selected-target role (#1637)."""

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
from project_host_static_archive_inputs import link_outcomes
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


A = 'header_a'
B = 'header_b'
HOST_HEADER = f'artifact("{A}", "header")'
TARGET_HEADER = f'artifact("{B}", "header")'
CHOICE = f'{{ host = {HOST_HEADER}, target = {TARGET_HEADER} }}'
UNRELATED = 'support/unrelated.cc'
INCLUDE = 'include'
WRITE_HEADER = r'''
role=$1
journal=$2
IFS=' ' read -r score < "$3" || exit 1
case "$score" in 17|23|29|41) ;; *) exit 1 ;; esac
printf '%s %s\n' "$role" "$score" >> "$journal" || exit 1
printf '#pragma once\n#include "config_side.hh"\ninline constexpr int config_score = %s;\n' "$score" > "$4" || exit 1
printf 'inline constexpr int config_role = %s;\nstatic_assert(config_role == side_role);\n' "$role" >> "$4" || exit 1
printf 'inline constexpr int side_role = %s;\n' "$role" > "$5" || exit 1
if [ "$#" = 6 ]; then
    printf 'inline constexpr int fallback_role = 7;\n' > "$6" || exit 1
fi
'''


def prepare(scope, journal, generated, host, *, traversal='both'):
    definition = prepare_diamond(scope, journal)
    header_uses = CHOICE if traversal == 'target' else f'{CHOICE}, {HOST_HEADER}'
    common_source = f'sources = ["{COMMON}"]\n    public_include_dirs = ["{INCLUDE}"]'
    assert definition.count(common_source) == 1, definition
    definition = definition.replace(common_source, f'''\
sources = ["{COMMON}"]
    generated_headers = [{header_uses}]
    deps = ["inc"]''')
    if traversal == 'host':
        app = 'sources = ["main.cc"]\n    deps = ["left", "right"]'
        assert definition.count(app) == 1, definition
        definition = definition.replace(app, 'sources = ["main.cc"]')
    if traversal == 'target':
        definition = definition.replace(f'    runtime_assets = [{NOTE}]\n', '')
    definition = definition.replace(f'    generated_headers = [{GENERATED_HEADER}]\n', '')
    scanner = f'sources = ["{SCANNER}"]\n    deps = ["left", "right"]'
    assert definition.count(scanner) == 1, definition
    definition = definition.replace(scanner, f'sources = ["{SCANNER}"]\n    deps = ["left", "right", "unrelated"]')
    definition += f'''\

cpp.library "inc" {{
    public_include_dirs = ["{INCLUDE}"]
}}
cpp.library "unrelated" {{
    sources = ["{UNRELATED}"]
}}
'''
    for name, role, score in ((A, 1, 17), (B, 2, 29)):
        (scope / f'{name}.seed').write_text(f'{score}\n', encoding='utf-8')
        fallback_arg = ', output("fallback")' if name == A else ''
        fallback_output = ' fallback = "fallback_role.hh"' if name == A else ''
        definition += f'''\

generate "{name}" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(WRITE_HEADER)}, "emit-config", "{role}", {json.dumps(str(generated))},
            input("seed"), output("header"), output("sibling"){fallback_arg}]
    inputs = {{ seed = "{name}.seed" }}
    outputs = {{ header = "config.hh" sibling = "config_side.hh"{fallback_output} }}
}}
'''
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    fallback = 7 if traversal == 'target' else 99
    (scope / INCLUDE / 'fallback_role.hh').write_text(f'inline constexpr int fallback_role = {fallback};\n',
                                                      encoding='utf-8')
    (scope / COMMON).write_text('''\
#include "score_api.hh"
#include "config.hh"
#include "fallback_role.hh"
static_assert(fallback_role == 7);
int common_arch() { return compile_arch; }
int common_score() { return config_score; }
''', encoding='utf-8')
    (scope / UNRELATED).write_text('''\
#if __has_include("config.hh")
#error inherited role header leaked into unrelated host library
#endif
int unrelated_score() { return 0; }
''', encoding='utf-8')
    arch = {LINUX: '__x86_64__', DARWIN: '__aarch64__'}[host]
    (scope / SCANNER).write_text(f'''\
#if !defined({arch})
#error the scanner must run on the host
#endif
#include "score_api.hh"
int unrelated_score();
int main() {{
    if (common_arch() != compile_arch) return 2;
    return left_score() + right_score() - common_score() - 3 + unrelated_score();
}}
''', encoding='utf-8')
    main = 'int main() { return 0; }\n' if traversal == 'host' else '''\
#include "score_api.hh"
int main() {
    if (common_arch() != compile_arch) return 2;
    return left_score() + right_score() - common_score() - 3;
}
'''
    (scope / 'main.cc').write_text(main, encoding='utf-8')
    return definition


def check_positions(plan, report, selected):
    definition = plan['definition']
    choice = definition.index(CHOICE)
    host_site = definition.index(HOST_HEADER, choice)
    target_site = definition.index(TARGET_HEADER, choice)
    plain_site = definition.index(HOST_HEADER, choice + len(CHOICE))
    inc_site = definition.index(f'"{INCLUDE}"', definition.index('public_include_dirs ='))
    disk = (INCLUDE, 'inherited from inc (declared in public_include_dirs)', inc_site)
    namespace_dir = namespace(selected) / 'virtual'
    expected = {
        'host': [(str(HOST_VIRTUAL / A), f'host = {HOST_HEADER}', host_site), disk],
        'target': [(str(namespace_dir / B), f'target = {TARGET_HEADER}', target_site),
                   (str(namespace_dir / A), HOST_HEADER, plain_site), disk],
    }
    parsed = PlanHTML(report)
    graph_rows = parsed.section('facts', 'Plan facts').descendants
    for owner, roles, parents in (
        ('common', ('host', 'target'), ()),
        ('left', ('host', 'target'), ('common',)),
        ('right', ('host', 'target'), ('common',)),
        ('scanner', ('host',), ('common', 'left')),
        ('app', ('target',), ('common', 'left')),
    ):
        wanted = {}
        for role in roles:
            for index, (path, provenance, site) in enumerate(expected[role]):
                name = f'{role} {index}' if len(roles) == 2 else str(index)
                for parent in parents:
                    provenance = f'inherited from {parent} ({provenance})'
                where = manifest_location(definition, site)
                wanted[name] = (path, provenance, where['line'], where['column'])

        facts = [row for row in plan['facts'] if (row['kind'], row['target']) == ('include', owner)]
        found = {row['name']: (row['value'], row['origin'], row['line'], row['column']) for row in facts}
        assert len(facts) == len(found) and found == wanted, (owner, found, wanted)
        view = {}
        for row in graph_rows:
            if (row.attrs.get('data-graph-kind'), row.attrs.get('data-graph-target')) != ('include', owner):
                continue
            label, = (cell.visible() for cell in row.descendants if cell.attrs.get('class') == 'fact-name')
            value, = (cell.visible() for cell in row.descendants if cell.attrs.get('class') == 'fact-value')
            assert label not in view, (owner, label)
            view[label] = value
        assert view == {name: value[0] for name, value in wanted.items()}, (owner, view)


def inspect(ycxx, scope, env, host, selected, journals, *, traversal='both'):
    plan, report = reports(ycxx, scope, env, selected, *journals)
    definition = plan['definition']
    target, = (row for row in plan['facts'] if row['kind'] == 'native-target')
    assert (target['target'], target['value']) == ('app', selected), target
    deps = [('left', 'common'), ('right', 'common'), ('common', 'inc')]
    choice = definition.index(CHOICE)
    edges = []
    if traversal != 'target':
        assert fact(plan, 'host-target', 'scanner', host)['name'] == 'triple'
        deps += [('scanner', name) for name in ('left', 'right', 'unrelated')]
        edges = [
            dependency_edge(definition, 'app', 'scan_config', 'runtime_assets', NOTE, output='note'),
            dependency_edge(definition, 'scan_config', 'scanner', 'inputs', REFERENCE,
                            binding='scanner', output='executable'),
            dependency_edge(definition, 'common', A, 'generated_headers', HOST_HEADER, start=choice,
                            binding='host', output='header'),
        ]
    if traversal != 'host':
        deps += [('app', 'left'), ('app', 'right')]
        edges.append(dependency_edge(definition, 'common', B, 'generated_headers', TARGET_HEADER, start=choice,
                                     binding='target', output='header'))
    if traversal != 'target':
        edges.append(dependency_edge(definition, 'common', A, 'generated_headers', HOST_HEADER,
                                     start=choice + len(CHOICE), output='header'))
    edges += [
        dependency_edge(definition, consumer, producer, 'deps', json.dumps(producer),
                        start=definition.index(f'"{consumer}" {{'))
        for consumer, producer in deps
    ]
    check_dependency_graph(plan, edges)
    keys = Counter((row['kind'], row['target'], row['name']) for row in plan['facts'])
    assert all(count == 1 for count in keys.values()), [key for key, count in keys.items() if count != 1]
    check_dependency_html(report, plan)
    check_target_dependencies_html(report, plan)
    check_target_graph_html(report, plan)
    if traversal == 'both':
        check_positions(plan, report, selected)
    return plan


def compare(ycxx, scope, env, selected, journals, plan):
    saved = scope.with_name('saved-role-include-plan.json')
    facts = plan['facts']
    position, = (row for row in facts if (row['kind'], row['target'], row['name']) ==
                 ('include', 'common', 'target 2'))
    retired = {**position, 'value': 'retired-include'}
    expected = [
        (plan, []),
        ({**plan, 'facts': [retired if row is position else row for row in facts]},
         [{'change': 'changed', 'before': retired, 'after': position}]),
    ]
    for snapshot, changes in expected:
        saved.write_text(json.dumps(snapshot), encoding='utf-8')
        before = scope_state(scope)
        logged = [journal_contents(journal) for journal in journals]
        args = ('show', 'app', *target_args(selected), '--against', str(saved))
        data = run_client(ycxx, scope, env, *args, '--format=json')
        report = run_client(ycxx, scope, env, *args, '--format=html')
        assert data.returncode == report.returncode == 0, (data.stderr, report.stderr)
        diff = json.loads(data.stdout)
        assert diff['changes'] == changes and diff['dependency_changes'] == [], diff
        section = PlanHTML(report.stdout).section('changes', 'Plan changes')
        rows = [node for node in section.descendants if node.tag == 'li']
        if changes:
            row, = rows
            assert all(term in row.visible() for term in ('common', 'target 2', 'retired-include')), row.visible()
            link, = (node for node in row.descendants if node.tag == 'a')
            assert link.attrs['href'] == f'#manifest-line-{position["line"]}', link.attrs
        else:
            assert not rows and 'No changes.' in section.visible(), section.visible()
        check_target_graph_html(report.stdout, plan)
        check_positions(plan, report.stdout, selected)
        assert scope_state(scope) == before and [journal_contents(journal) for journal in journals] == logged


def check_watch(ycxx, scope, env, host, other, journals):
    scanner_runs, generated = journals
    watch = Watch(ycxx, scope, env, *target_args(other))
    try:
        summary, events, watched = watch.build('watch both role-bound header producers')
        assert summary['actions_executed'] == 0, summary
        assert {str(scope / path) for path in (COMMON, UNRELATED, f'{A}.seed', f'{B}.seed')} <= set(watched), watched
        check_external_actions(events)
        initial = published(scope, other)

        replace_source(scope, f'{B}.seed', '41\n')
        _, events, _ = watch.build('edit only the selected role header')
        check_external_actions(events, B)
        assert action_keys(events, 'Link') == ['app'], events
        assert not RAN & (tu_outcomes(events, SCANNER) | tu_outcomes(events, Path(UNRELATED).name)), events
        assert 'executed' in tu_outcomes(events, Path(COMMON).name), events
        target_updated = published(scope, other)
        assert target_updated[0][0] != initial[0][0] and target_updated[1] == initial[1]
        assert journal_contents(scanner_runs) == '17\n'
        check_binary(scope, other, other, 17, host)

        replace_source(scope, f'{A}.seed', '23\n')
        _, events, _ = watch.build('edit the host header while the selected config still wins')
        check_external_actions(events, A, 'scan_config')
        assert 'executed' in link_outcomes(events, 'scanner'), events
        assert not RAN & tu_outcomes(events, Path(UNRELATED).name), events
        latest = published(scope, other)
        assert latest[0] == target_updated[0] and latest[1][0] != target_updated[1][0]
        assert journal_contents(scanner_runs) == '17\n23\n'
        check_binary(scope, other, other, 23, host)

        replace_source(scope, f'{A}.seed', '  23  \n')
        _, events, _ = watch.build('keep both compile inputs when emitted header bytes are equal')
        check_external_actions(events, A)
        for path in (COMMON, LEFT, RIGHT, SCANNER, UNRELATED, 'main.cc'):
            assert not RAN & tu_outcomes(events, Path(path).name), path
        assert published(scope, other) == latest
        assert Counter(journal_contents(generated).splitlines()) == Counter(
            ('1 17', '2 29', '2 41', '1 23', '1 23'))
        watch.idle('watch after equal generated headers')
    finally:
        watch.close()


def check_diamond(ycxx, scope, env, host, other, *, native=False):
    selected = host if native else other
    journals = tuple(scope.with_name(f'{scope.name}-{name}-runs') for name in ('scanner', 'header'))
    scanner_runs, generated = journals
    prepare(scope, *journals, host)
    cold, _ = reports(ycxx, scope, env, selected, *journals)
    check_dependency_graph(cold, [], complete=False)
    assert not (scope / '.ycxx').exists()
    _, events = build(ycxx, scope, env, selected, selected, 'compile the same library diamond for both roles')
    check_external_actions(events, A, B, 'scan_config')
    assert action_keys(events, 'Link') == ['app', 'scanner'], events
    assert journal_contents(scanner_runs) == '17\n'
    assert Counter(journal_contents(generated).splitlines()) == Counter(('1 17', '2 29'))
    if native:
        check_program(scope / executable(selected), 29)
    else:
        check_binary(scope, other, other, 17, host)
    plan = inspect(ycxx, scope, env, host, selected, journals)
    if not native:
        compare(ycxx, scope, env, selected, journals, plan)
        check_watch(ycxx, scope, env, host, selected, journals)


def check_single(ycxx, scope, env, host, other, *, traversal):
    scanner_runs, generated = journals = tuple(scope.with_name(f'{scope.name}-{name}-runs')
                                                for name in ('scanner', 'header'))
    prepare(scope, *journals, host, traversal=traversal)
    host_only = traversal == 'host'
    inactive = B if host_only else A
    (scope / f'{inactive}.seed').write_text('an incorrect effect must fail\n', encoding='utf-8')
    selected = other if host_only else host
    _, events = build(ycxx, scope, env, selected, selected, 'run only the reached role header producer')
    if host_only:
        check_external_actions(events, A, 'scan_config')
        assert journal_contents(scanner_runs) == '17\n'
        assert journal_contents(generated) == '1 17\n'
        check_binary(scope, other, other, 17, host)
    else:
        check_external_actions(events, B)
        assert not scanner_runs.exists() and journal_contents(generated) == '2 29\n'
        check_program(scope / executable(selected), 29)
    inspect(ycxx, scope, env, host, selected, journals, traversal=traversal)


def check_partial(ycxx, scope, env, host, other):
    scope.mkdir()
    (scope / 'inc').mkdir()
    (scope / 'inc/score.hh').write_text('inline constexpr int score = 17;\n', encoding='utf-8')
    (scope / 'main.cc').write_text('#include "score.hh"\nint support(); int main() { return score + support(); }\n',
                                   encoding='utf-8')
    (scope / 'support.cc').write_text('int support() { return 0; }\n', encoding='utf-8')
    definition = '''\
cpp.executable "app" {
    sources = ["main.cc"]
    public_include_dirs = ["inc"]
    deps = ["support"]
}
cpp.library "support" {
    sources = ["support.cc"]
}
'''
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    build(ycxx, scope, env, other, other, 'warm the selected app before refreshing its library declaration')
    (scope / 'fresh.cc').write_text('int support() { return 0; }\n', encoding='utf-8')
    replace_source(scope, 'build.ycxx', definition.replace('"support.cc"', '"fresh.cc"'))

    plan, report = reports(ycxx, scope, env, other)
    assert plan['dependency_graph']['complete'] is False, plan
    assert fact(plan, 'unresolved', 'support', 'standard/rules')['name'] == 'cpp.library'
    include = fact(plan, 'include', 'app', 'inc')
    assert (include['name'], include['origin']) == ('0', 'declared in public_include_dirs'), include
    site = manifest_location(definition, definition.index('"inc"'))
    assert (include['line'], include['column']) == (site['line'], site['column']), include
    check_target_graph_html(report, plan)


def check_rejection(ycxx, scope, env, host, other, *, variant):
    journals = tuple(scope.with_name(f'{scope.name}-{name}-runs') for name in ('scanner', 'header'))
    definition = prepare(scope, *journals, host)
    if variant == 'missing':
        definition = definition.replace(CHOICE, f'{{ host = {HOST_HEADER} }}')
        diagnostic = "generated_headers binding requires exactly 'host' and 'target' fields"
    elif variant == 'extra':
        definition = definition.replace(CHOICE, f'{{ host = {HOST_HEADER}, target = {TARGET_HEADER}, extra = {HOST_HEADER} }}')
        diagnostic = "generated_headers binding requires exactly 'host' and 'target' fields"
    elif variant == 'wrong-arm':
        definition = definition.replace(CHOICE, f'{{ host = {HOST_HEADER}, target = "config.hh" }}')
        diagnostic = "expected an artifact reference for 'target' in generated_headers"
    elif variant == 'direct':
        source = 'sources = ["main.cc"]'
        definition = definition.replace(source, f'{source}\n    generated_headers = [{CHOICE}]')
        diagnostic = 'expected an artifact reference'
    else:
        source = f'seed = "{A}.seed"'
        assert definition.count(source) == 1, definition
        definition = definition.replace(source, f'{source} helper = artifact("second_tool", "executable")')
        definition += '\ncpp.host_executable "second_tool" { sources = ["second.cc"] }\n'
        (scope / 'second.cc').write_text('int main() { return 0; }\n', encoding='utf-8')
        diagnostic = 'generated headers for host executables require a producer chain without host executable inputs'
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    summary, events = traced_build(ycxx, scope, env, f'reject the invalid {variant} header binding',
                                   *target_args(other), succeeds=False)
    assert any(diagnostic in message for message in summary['diagnostics']), summary
    check_external_actions(events)
    assert not any(event.get('kind') in {'Tu', 'Link'} for event in events), events
    assert not any(path.exists() for path in (*journals, scope / executable(other)))


def main():
    ycxx = Path(sys.argv[1]).resolve()
    host, other = {'linux': (LINUX, DARWIN), 'darwin': (DARWIN, LINUX)}[sys.platform]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-role-bound-headers-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        env = env_for(runtime)
        checks = (
            ('cross', check_diamond, {}),
            ('native', check_diamond, {'native': True}),
            ('host-only', check_single, {'traversal': 'host'}),
            ('target-only', check_single, {'traversal': 'target'}),
            ('partial', check_partial, {}),
            *((variant, check_rejection, {'variant': variant})
              for variant in ('missing', 'extra', 'wrong-arm', 'direct', 'host-dependent')),
        )
        for name, check, options in checks:
            scope = root / name
            try:
                check(ycxx, scope, env, host, other, **options)
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
