"""Link ordered archives inherited by native host tools through C++ libraries (#1622)."""

import json
from pathlib import Path
import sys
import tempfile

from project_build import action_keys, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_host_generated_sources import reports
from project_host_static_archive_inputs import FILENAME, HOST_VIRTUAL, ORDER, SELECTOR, link_outcomes, prepare_archives
from project_host_static_archive_inputs import prepare as prepare_direct, references
from project_inspection import (
    check_dependency_graph, check_dependency_html, check_target_dependencies_html, check_target_graph_html,
    dependency_edge, fact,
)
from project_native_host_tool import GENERATED_HEADER, NOTE, REFERENCE, SCANNER
from project_shader_bundle import check_external_actions
from project_targets import (
    DARWIN, LINUX, RAN, build, check_binary, executable, journal_contents, namespace, published, target_args,
)
from runtime_dir import runtime_root


Z = 'z_provider'
A = 'a_provider'
OPERANDS = (('scanner', Z), ('left', A), ('right', A), ('common', Z), ('common', Z))
LIBRARY_UNITS = ('left.cc', 'right.cc', 'common.cc')
EMPTY_ARCHIVE = b'!<arch>\n'


def archive_field(providers, name='link_inputs'):
    return f'    {name} = [\n{references(providers)}\n    ]\n'


def prepare_diamond(scope, archive_dir, scanner_runs, archive_runs, host):
    definition = prepare_direct(scope, archive_dir, scanner_runs, archive_runs, host)
    original = archive_field(ORDER)
    assert definition.count(original) == 1, definition
    definition = definition.replace(original, '    deps = ["left", "right"]\n' + archive_field((Z,)))
    definition += f'''\

cpp.library "left" {{
    sources = ["left.cc"]
    deps = ["common"]
{archive_field((A,), 'public_link_inputs')}}}
cpp.library "right" {{
    sources = ["right.cc"]
    deps = ["common"]
{archive_field((A,), 'public_link_inputs')}}}
cpp.library "common" {{
    sources = ["common.cc"]
{archive_field((Z, Z), 'public_link_inputs')}}}
'''
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    for source, code in (
        ('left.cc', 'int common_score(); int left_score() { return common_score() + 1; }'),
        ('right.cc', 'int common_score(); int right_score() { return common_score() + 2; }'),
        ('common.cc', 'extern "C" int choose(); int common_score() { return choose(); }'),
    ):
        (scope / source).write_text(code + '\n', encoding='utf-8')
    scanner = scope / SCANNER
    code = scanner.read_text(encoding='utf-8')
    start = code.index('extern "C" int choose();')
    scanner.write_text(code[:start] + '''\
int left_score();
int right_score();
int common_score();
int main() { return left_score() + right_score() - common_score() - 3; }
''', encoding='utf-8')
    return definition


def prepare_shared(scope, archive_dir, scanner_runs, archive_runs, host, *, empty):
    definition = prepare_direct(scope, archive_dir, scanner_runs, archive_runs, host)
    original = archive_field(ORDER)
    assert definition.count(original) == 1, definition
    definition = definition.replace(original, '    deps = ["shared"]\n')
    source = 'sources = ["main.cc"]'
    assert definition.count(source) == 1, definition
    definition = definition.replace(source, f'{source}\n    deps = ["shared"]')
    definition += f'''\

cpp.library "shared" {{
    sources = ["shared.cc"]
{archive_field((A,), 'public_link_inputs')}}}
'''
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    shared = 'int shared_score() { return 17; }' if empty else (
        'extern "C" int choose(); int shared_score() { return choose(); }')
    (scope / 'shared.cc').write_text(shared + '\n', encoding='utf-8')
    scanner = scope / SCANNER
    code = scanner.read_text(encoding='utf-8')
    start = code.index('extern "C" int choose();')
    scanner.write_text(code[:start] + 'int shared_score(); int main() { return shared_score(); }\n', encoding='utf-8')
    (scope / 'main.cc').write_text('''\
#include "scan_score.hh"
int shared_score();
int main() { return shared_score() == scanned_score ? scanned_score : 1; }
''', encoding='utf-8')
    if empty:
        (scope / 'archives/alternative.a').write_bytes(EMPTY_ARCHIVE)


def archive_edges(plan, target, directory, uses):
    definition = plan['definition']
    starts = {}
    edges = []
    rows = sorted((row for row in plan['facts'] if row['kind'] == 'link-input' and row['target'] == target),
                  key=lambda row: int(row['name']))
    assert len(rows) == len(uses), (target, rows)
    for index, (row, (owner, producer)) in enumerate(zip(rows, uses)):
        reference = f'artifact("{producer}", "archive")'
        usage = 'link_inputs' if owner == target else 'public_link_inputs'
        origin = f'declared in {usage} as {reference}'
        if owner != target:
            origin = f'inherited from {owner} ({origin})'
        start = starts.get(owner, definition.index(f'"{owner}" {{'))
        edge = dependency_edge(definition, owner, producer, usage, reference, start=start, output='archive')
        starts[owner] = definition.index(reference, start) + len(reference)
        location = edge['location']
        assert (row['name'], row['value'], row['origin']) == (str(index), str(directory / producer / FILENAME), origin), row
        assert (row['line'], row['column']) == (location['line'], location['column']), row
        edges.append(edge)
    return edges


def check_plan(ycxx, scope, env, host, selected, journal, runs, *, shared=False):
    plan, report = reports(ycxx, scope, env, selected, journal, runs)
    definition = plan['definition']
    target, = (row for row in plan['facts'] if row['kind'] == 'native-target')
    assert (target['target'], target['value']) == ('app', selected or host), target
    assert fact(plan, 'host-target', 'scanner', host)['name'] == 'triple'
    if shared:
        deps = (('app', 'shared'), ('scanner', 'shared'))
        uses = (('shared', A),)
        archives = archive_edges(plan, 'scanner', HOST_VIRTUAL, uses)
        assert archives == archive_edges(plan, 'app', namespace(selected) / 'virtual', uses)
    else:
        deps = (('scanner', 'left'), ('scanner', 'right'), ('left', 'common'), ('right', 'common'))
        archives = archive_edges(plan, 'scanner', HOST_VIRTUAL, OPERANDS)
        assert not any(row['kind'] == 'link-input' and row['target'] == 'app' for row in plan['facts']), plan
    links = [
        dependency_edge(definition, owner, producer, 'deps', json.dumps(producer),
                        start=definition.index(f'"{owner}" {{'))
        for owner, producer in deps
    ]
    check_dependency_graph(plan, [
        dependency_edge(definition, 'app', 'scan_config', 'generated_headers', GENERATED_HEADER, output='header'),
        dependency_edge(definition, 'app', 'scan_config', 'runtime_assets', NOTE, output='note'),
        dependency_edge(definition, 'scan_config', 'scanner', 'inputs', REFERENCE,
                        output='executable', binding='scanner'),
        *links, *archives,
    ])
    check_dependency_html(report, plan)
    check_target_dependencies_html(report, plan)
    check_target_graph_html(report, plan)


def check_host_units_reused(events):
    outcomes = {unit: tu_outcomes(events, unit) for unit in (SCANNER, *LIBRARY_UNITS)}
    assert not any(RAN & states for states in outcomes.values()), outcomes


def check_watch(ycxx, scope, env, host, other, journal, runs):
    watch = Watch(ycxx, scope, env, *target_args(other))
    try:
        summary, events, watched = watch.build('watch inherited archive selection')
        assert summary['actions_executed'] == 0, summary
        assert {str(scope / path) for path in (SELECTOR, 'archives/alternative.a', *LIBRARY_UNITS)} <= set(watched), watched
        check_host_units_reused(events)

        replace_source(scope, SELECTOR, 'two\n')
        _, events, _ = watch.build('relink the host diamond after an archive-only edit')
        check_external_actions(events, Z, 'scan_config')
        assert 'executed' in link_outcomes(events, 'scanner'), events
        check_host_units_reused(events)
        check_binary(scope, other, other, 23, host)
        good = published(scope, other)

        replace_source(scope, SELECTOR, 'incompatible\n')
        summary, events, watched = watch.build('reject the extracted member in the host archive', succeeds=False)
        check_external_actions(events, Z)
        check_host_units_reused(events)
        assert any(str(HOST_VIRTUAL / Z / FILENAME) in diagnostic and 'one.o' in diagnostic
                   and 'host executable' in diagnostic for diagnostic in summary['diagnostics']), summary
        assert str(scope / SELECTOR) in watched, watched
        assert published(scope, other) == good
        assert journal_contents(journal) == '17\n23\n'

        replace_source(scope, SELECTOR, 'three\n')
        _, events, _ = watch.build('recover the host diamond with a valid archive')
        check_external_actions(events, Z, 'scan_config')
        assert 'executed' in link_outcomes(events, 'scanner'), events
        check_host_units_reused(events)
        check_binary(scope, other, other, 41, host)
        assert journal_contents(journal) == '17\n23\n41\n'
        check_plan(ycxx, scope, env, host, other, journal, runs)
        watch.idle('watch after recovering the host archive')
    finally:
        watch.close()


def check_diamond(ycxx, scope, env, archive_dir, host, other):
    journal, runs = (scope.with_name(f'diamond-{name}-runs') for name in ('scanner', 'archive'))
    prepare_diamond(scope, archive_dir, journal, runs, host)
    cold, _ = reports(ycxx, scope, env, other, journal, runs)
    check_dependency_graph(cold, [], complete=False)
    assert not (scope / '.ycxx').exists()
    _, events = build(ycxx, scope, env, other, other, 'link direct and inherited archives into the host scanner')
    check_external_actions(events, Z, A, 'scan_config')
    assert all('executed' in tu_outcomes(events, source) for source in (SCANNER, *LIBRARY_UNITS)), events
    assert action_keys(events, 'Link') == ['app', 'scanner'], events
    assert journal_contents(journal) == '17\n' and journal_contents(runs) == f'{host} {other} one\n'
    check_binary(scope, other, other, 17, host)
    check_plan(ycxx, scope, env, host, other, journal, runs)
    initial = published(scope, other)

    replace_source(scope, SELECTOR, '  one  \n')
    _, events = build(ycxx, scope, env, other, other, 'reuse the link when an inherited archive stays byte-identical')
    check_external_actions(events, Z)
    check_host_units_reused(events)
    assert not RAN & link_outcomes(events, 'scanner'), events
    assert published(scope, other) == initial

    replace_source(scope, SELECTOR, 'equal-image\n')
    _, events = build(ycxx, scope, env, other, other, 'relink changed archive bytes and reuse the scanner effect')
    check_external_actions(events, Z)
    check_host_units_reused(events)
    assert RAN & link_outcomes(events, 'scanner'), events
    assert not RAN & tu_outcomes(events, 'main.cc'), events
    assert published(scope, other) == initial and journal_contents(journal) == '17\n'
    check_watch(ycxx, scope, env, host, other, journal, runs)


def check_shared(ycxx, scope, env, archive_dir, host, other, *, cross):
    journal, runs = (scope.with_name(f'{scope.name}-{name}-runs') for name in ('scanner', 'archive'))
    prepare_shared(scope, archive_dir, journal, runs, host, empty=cross)
    selected = other if cross else None
    status = 17 if cross else 31
    _, events = build(ycxx, scope, env, selected, selected or host, 'share one named archive receipt between both links')
    check_external_actions(events, A, 'scan_config')
    assert action_keys(events, 'Link') == ['app', 'scanner'], events
    assert journal_contents(journal) == f'{status}\n', journal
    check_binary(scope, selected, selected or host, status, host)
    check_plan(ycxx, scope, env, host, selected, journal, runs, shared=True)
    if not cross:
        return

    good = published(scope, other)
    main = (scope / 'main.cc').read_text(encoding='utf-8')
    archive = scope / 'archives/alternative.a'
    archive.write_bytes((archive_dir / archive.name).read_bytes())
    replace_source(scope, 'main.cc', 'extern "C" int choose();\n' +
                   main.replace('? scanned_score :', '? choose() :'))
    summary, events = traced_build(ycxx, scope, env, 'the selected linker rejects an incompatible shared archive',
                                   *target_args(other), succeeds=False)
    check_external_actions(events, A)
    assert RAN & link_outcomes(events, 'scanner'), events
    assert any(str(namespace(other) / 'virtual' / A / FILENAME) in message
               and 'host executable' not in message for message in summary['diagnostics']), summary
    assert published(scope, other) == good and journal_contents(journal) == '17\n'

    archive.write_bytes(EMPTY_ARCHIVE)
    replace_source(scope, 'main.cc', main)
    build(ycxx, scope, env, other, other, 'recover the cross link with the portable empty archive')
    assert published(scope, other) == good and journal_contents(journal) == '17\n'


def check_rejection(ycxx, scope, env, archive_dir, host, other, *, cycle):
    journal, runs = (scope.with_name(f'{scope.name}-{name}-runs') for name in ('scanner', 'archive'))
    definition = prepare_diamond(scope, archive_dir, journal, runs, host)
    direct = archive_field((Z,))
    assert definition.count(direct) == 1, definition
    definition = definition.replace(direct, '')
    selector = f'selector = "{SELECTOR}"'
    assert definition.count(selector) == 1, definition
    if cycle:
        helper = REFERENCE
        diagnostic = 'host executable generated archive dependency cycle'
    else:
        helper = 'artifact("second_tool", "executable")'
        definition += '\ncpp.host_executable "second_tool" { sources = ["second.cc"] }\n'
        (scope / 'second.cc').write_text('int main() { return 0; }\n', encoding='utf-8')
        diagnostic = 'generated archives for host executables require a producer chain without host executable inputs'
    (scope / 'build.ycxx').write_text(definition.replace(selector, f'{selector} helper = {helper}'), encoding='utf-8')
    summary, events = traced_build(ycxx, scope, env, 'reject a host dependency in an inherited archive producer',
                                   *target_args(other), succeeds=False)
    assert any(diagnostic in message for message in summary['diagnostics']), summary
    check_external_actions(events)
    assert not any(event.get('kind') in {'Tu', 'Link'} for event in events), events
    assert not any(path.exists() for path in (journal, runs, scope / executable(other)))


def main():
    ycxx, archiver = (Path(arg).resolve() for arg in sys.argv[1:3])
    host, other = {'linux': (LINUX, DARWIN), 'darwin': (DARWIN, LINUX)}[sys.platform]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-host-library-static-archive-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        env = env_for(runtime)
        archive_dir = prepare_archives(ycxx, archiver, root, env, host)
        checks = (
            ('diamond', check_diamond, {}),
            ('native', check_shared, {'cross': False}),
            ('cross', check_shared, {'cross': True}),
            ('cycle', check_rejection, {'cycle': True}),
            ('host-dependent', check_rejection, {'cycle': False}),
        )
        for name, check, options in checks:
            scope = root / name
            try:
                check(ycxx, scope, env, archive_dir, host, other, **options)
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
