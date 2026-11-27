"""Link ordered named archives into a native tool for a foreign project (#1616)."""

import json
from pathlib import Path
import shlex
import shutil
import sys
import tempfile

from project_build import action_keys, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_host_generated_sources import reports
from project_host_planning_inputs import COPY as INVENTORY, prepare as prepare_planning
from project_inspection import (
    check_dependency_graph, check_dependency_html, check_target_dependencies_html, check_target_graph_html,
    check_value_bindings_html, dependency_edge, fact,
)
from project_link_inputs import checked_command
from project_native_host_tool import GENERATED_HEADER, NOTE, REFERENCE, SCANNER, prepare as prepare_tool
from project_native_host_witness import prepare as prepare_witness
from project_shader_bundle import check_external_actions
from project_staged_witness import check_observation, score_value
from project_targets import (
    DARWIN, LINUX, RAN, build, check_binary, executable, journal_contents, namespace, published, target_args,
)
from runtime_dir import runtime_root


SELECTOR = 'archive-selection.txt'
FILENAME = 'provider.a'
HOST_VIRTUAL = Path('.ycxx/host/virtual')
SENTINEL = 'archive_only.hh'
ORDER = ('z_provider', 'a_provider', 'z_provider')
REVERSED = ('a_provider', 'z_provider', 'a_provider')
SELECT = r'''
IFS=' ' read -r selection < "$1" || exit 1
case "$selection" in
    one) /bin/cat "$3" > "$8" ;;
    equal-image) /bin/cat "$4" > "$8" ;;
    two) /bin/cat "$5" > "$8" ;;
    three) /bin/cat "$6" > "$8" ;;
    incompatible) /bin/cat "$7" > "$8" ;;
    *) exit 1 ;;
esac || exit 1
printf '#error a link-only output entered preprocessing\n' > "$9" || exit 1
printf '%s %s %s\n' "$BUILD_TRIPLE" "$TARGET_TRIPLE" "$selection" >> "$2"
'''


def prepare_archives(ycxx, archiver, root, env, host):
    objects = root / 'members'
    archives = root / 'archives'
    objects.mkdir()
    archives.mkdir()
    standalone = dict(env, YCXX_NO_DAEMON='1')
    standalone.pop('YCXX_DAEMON', None)
    members = {
        'finish': 'extern "C" int finish() { return 1; }',
        'bridge': 'extern "C" int finish(); extern "C" int bridge() { return finish(); }',
        'alternative': 'extern "C" int choose() { return 31; }',
        'unused': 'extern "C" int never_extracted() { return 89; }',
    }
    for name, score in (('one', 16), ('two', 22), ('three', 40)):
        members[name] = f'extern "C" int bridge(); extern "C" int choose() {{ return bridge() + {score}; }}'
    for name, source in members.items():
        (objects / f'{name}.cc').write_text(source + '\n', encoding='utf-8')
        checked_command(ycxx, 'cxx', '-c', f'{name}.cc', '-o', f'{name}.o', cwd=objects, env=standalone)
    packs = {
        'one': ('one.o', 'finish.o'),
        'equal-image': ('one.o', 'finish.o', 'unused.o'),
        'two': ('two.o', 'finish.o'),
        'three': ('three.o', 'finish.o'),
        'alternative': ('alternative.o', 'bridge.o'),
    }
    for name, inputs in packs.items():
        checked_command(archiver, 'rcs', archives / f'{name}.a', *inputs, cwd=objects, env=standalone)

    # Keep the native archive index and symbol unchanged, but make the selected member's object
    # header claim another CPU. The same indexed choose() must then fail at extraction.
    member = (objects / 'one.o').read_bytes()
    packed = (archives / 'one.a').read_bytes()
    assert packed.count(member) == 1
    if host == LINUX:
        assert member[:4] == b'\x7fELF' and member[18:20] == b'\x3e\x00'
        incompatible = member[:18] + b'\xb7\x00' + member[20:]
    else:
        assert member[:8] == bytes.fromhex('cffaedfe0c000001')
        incompatible = member[:4] + bytes.fromhex('07000001') + member[8:]
    (archives / 'incompatible.a').write_bytes(packed.replace(member, incompatible))
    assert (archives / 'equal-image.a').read_bytes() != packed
    return archives


def references(providers):
    return ',\n'.join(f'        artifact("{provider}", "archive")' for provider in providers)


def attach_archives(scope, archive_dir, runs, definition, host):
    field = f'sources = ["{SCANNER}"]'
    assert definition.count(field) == 1, definition
    definition = definition.replace(field, f'{field}\n    link_inputs = [\n{references(ORDER)}\n    ]\n')
    definition += f'''\

generate "z_provider" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(SELECT)}, "select-archive", input("selector"), {json.dumps(str(runs))},
            input("one"), input("equal"), input("two"), input("three"), input("incompatible"),
            output("archive"), output("sentinel")]
    environment = {{ BUILD_TRIPLE = host_triple() TARGET_TRIPLE = target_triple() }}
    inputs = {{ selector = "{SELECTOR}", one = "archives/one.a", equal = "archives/equal-image.a",
                two = "archives/two.a", three = "archives/three.a", incompatible = "archives/incompatible.a" }}
    outputs = {{ archive = "{FILENAME}", sentinel = "{SENTINEL}" }}
}}
generate "a_provider" {{
    tool = "/bin/cp"
    args = [input("source"), output("archive")]
    inputs = {{ source = "archives/alternative.a" }}
    outputs = {{ archive = "{FILENAME}" }}
}}
'''
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    (scope / SELECTOR).write_text('one\n', encoding='utf-8')
    shutil.copytree(archive_dir, scope / 'archives')

    scanner = scope / SCANNER
    macro = {LINUX: '__x86_64__', DARWIN: '__aarch64__'}[host]
    absolute = json.dumps(str(scope / HOST_VIRTUAL / 'z_provider' / SENTINEL))
    scanner.write_text(f'''\
#if !defined({macro})
#error scanner compiled for the selected app instead of the host
#endif
#if __has_include("{SENTINEL}") || __has_include({absolute})
#error generated archive outputs leaked into preprocessing
#endif
extern "C" int choose();
int main() {{ return choose(); }}
''', encoding='utf-8')
    return definition


def prepare(scope, archive_dir, journal, runs, host):
    definition = attach_archives(scope, archive_dir, runs, prepare_tool(scope, journal), host)
    emitter = scope / 'emit-header.sh'
    script = emitter.read_text(encoding='utf-8')
    assert script.count('17|23)') == 1, script
    emitter.write_text(script.replace('17|23)', '17|23|31|41)'), encoding='utf-8')
    return definition


def archive_edges(plan, providers):
    definition = plan['definition']
    start = definition.index('link_inputs = [')
    edges = []
    inputs = sorted((row for row in plan['facts'] if row['kind'] == 'link-input' and row['target'] == 'scanner'),
                    key=lambda row: int(row['name']))
    assert len(inputs) == len(providers), inputs
    assert not any(row['kind'] == 'link-input' and row['target'] == 'app' for row in plan['facts']), plan
    for index, (provider, row) in enumerate(zip(providers, inputs)):
        reference = f'artifact("{provider}", "archive")'
        edge = dependency_edge(definition, 'scanner', provider, 'link_inputs', reference,
                               start=start, output='archive')
        start = definition.index(reference, start) + len(reference)
        location = edge['location']
        assert row['name'] == str(index) and row['value'] == str(HOST_VIRTUAL / provider / FILENAME), row
        assert row['origin'] == f'declared in link_inputs as {reference}', row
        assert (row['line'], row['column']) == (location['line'], location['column']), row
        assert not any(item['kind'] == 'include' and item['value'] == str(HOST_VIRTUAL / provider)
                       for item in plan['facts']), plan
        edges.append(edge)
    return edges


def check_plan(ycxx, scope, env, host, other, journal, runs, providers):
    plan, report = reports(ycxx, scope, env, other, journal, runs)
    definition = plan['definition']
    selection, = (row for row in plan['facts'] if row['kind'] == 'native-target')
    assert (selection['target'], selection['value']) == ('app', other), selection
    assert fact(plan, 'host-target', 'scanner', host)['name'] == 'triple'
    for name, value, origin in (('BUILD_TRIPLE', host, 'host_triple()'),
                                ('TARGET_TRIPLE', other, 'target_triple()')):
        item = fact(plan, 'environment', 'z_provider', value)
        assert (item['name'], item['origin']) == (name, origin), item
    check_dependency_graph(plan, [
        dependency_edge(definition, 'app', 'scan_config', 'generated_headers', GENERATED_HEADER, output='header'),
        dependency_edge(definition, 'app', 'scan_config', 'runtime_assets', NOTE, output='note'),
        dependency_edge(definition, 'scan_config', 'scanner', 'inputs', REFERENCE,
                        output='executable', binding='scanner'),
        *archive_edges(plan, providers),
    ])
    check_dependency_html(report, plan)
    check_target_dependencies_html(report, plan)
    check_target_graph_html(report, plan)


def link_outcomes(events, name):
    return {event['outcome'] for event in events if event.get('kind') == 'Link' and event.get('key') == name}


def check_watch(ycxx, scope, env, host, other, journal, runs):
    watch = Watch(ycxx, scope, env, *target_args(other))
    try:
        summary, events, watched = watch.build('watch the current native archive inputs')
        assert summary['actions_executed'] == 0, summary
        assert {str(scope / path) for path in (SELECTOR, SCANNER, 'archives/alternative.a',
                                                'archives/incompatible.a')} <= set(watched), watched
        check_external_actions(events)
        assert not RAN & link_outcomes(events, 'scanner'), events

        replace_source(scope, SELECTOR, 'two\n')
        _, events, _ = watch.build('an archive-only edit changes the extracted function')
        check_external_actions(events, 'z_provider', 'scan_config')
        assert 'executed' in link_outcomes(events, 'scanner'), events
        assert not RAN & tu_outcomes(events, SCANNER), events
        check_binary(scope, other, other, 23, host)
        installed = published(scope, other)
        assert journal_contents(journal) == '17\n23\n'

        replace_source(scope, SELECTOR, 'incompatible\n')
        summary, events, watched = watch.build('an extracted incompatible object fails the host link', succeeds=False)
        check_external_actions(events, 'z_provider')
        assert not RAN & tu_outcomes(events, SCANNER), events
        assert any(FILENAME in diagnostic and 'one.o' in diagnostic and 'host executable' in diagnostic
                   for diagnostic in summary['diagnostics']), summary
        assert str(scope / SELECTOR) in watched, watched
        assert published(scope, other) == installed and journal_contents(journal) == '17\n23\n'
        check_binary(scope, other, other, 23, host)

        replace_source(scope, SELECTOR, 'three\n')
        _, events, _ = watch.build('restore a compatible archive in one edit')
        check_external_actions(events, 'z_provider', 'scan_config')
        assert 'executed' in link_outcomes(events, 'scanner'), events
        assert not RAN & tu_outcomes(events, SCANNER), events
        check_binary(scope, other, other, 41, host)
        assert journal_contents(journal) == '17\n23\n41\n'
        check_plan(ycxx, scope, env, host, other, journal, runs, ORDER)
        watch.idle('archive watch after successful publication')
    finally:
        watch.close()


def check_builds(ycxx, scope, env, archive_dir, host, other):
    journal, runs = (scope.with_name(f'normal-{name}-runs') for name in ('scanner', 'archive'))
    definition = prepare(scope, archive_dir, journal, runs, host)
    cold, _ = reports(ycxx, scope, env, other, journal, runs)
    check_dependency_graph(cold, [], complete=False)
    assert not (scope / '.ycxx').exists()

    _, events = build(ycxx, scope, env, other, other, 'link authored repeated archive operands for a foreign build')
    check_external_actions(events, 'z_provider', 'a_provider', 'scan_config')
    assert 'executed' in tu_outcomes(events, SCANNER) and 'executed' in link_outcomes(events, 'scanner'), events
    assert journal_contents(journal) == '17\n'
    assert journal_contents(runs) == f'{host} {other} one\n'
    check_binary(scope, other, other, 17, host)
    check_plan(ycxx, scope, env, host, other, journal, runs, ORDER)
    installed = published(scope, other)

    replace_source(scope, SELECTOR, '  one  \n')
    _, events = build(ycxx, scope, env, other, other, 'reuse the link with byte-identical archive output')
    check_external_actions(events, 'z_provider')
    assert not RAN & (tu_outcomes(events, SCANNER) | link_outcomes(events, 'scanner')), events
    assert published(scope, other) == installed and journal_contents(journal) == '17\n'
    assert journal_contents(runs) == f'{host} {other} one\n' * 2

    replace_source(scope, SELECTOR, 'equal-image\n')
    _, events = build(ycxx, scope, env, other, other, 'relink with an unused archive member and reuse the host image')
    check_external_actions(events, 'z_provider')
    assert RAN & link_outcomes(events, 'scanner'), events
    assert not RAN & tu_outcomes(events, SCANNER) and not RAN & tu_outcomes(events, 'main.cc'), events
    assert published(scope, other) == installed and journal_contents(journal) == '17\n'

    check_watch(ycxx, scope, env, host, other, journal, runs)
    authored = references(ORDER)
    assert definition.count(authored) == 1, definition
    replace_source(scope, 'build.ycxx', definition.replace(authored, references(REVERSED)))
    _, events = build(ycxx, scope, env, other, other, 'replace only the authored archive ordering')
    check_external_actions(events, 'scan_config')
    assert 'executed' in link_outcomes(events, 'scanner') and not RAN & tu_outcomes(events, SCANNER), events
    check_binary(scope, other, other, 31, host)
    assert journal_contents(journal) == '17\n23\n41\n31\n'
    check_plan(ycxx, scope, env, host, other, journal, runs, REVERSED)


def check_in_flight(ycxx, scope, env, archive_dir, host, other):
    journal, runs = (scope.with_name(f'in-flight-{name}-runs') for name in ('scanner', 'archive'))
    sentinel = scope.with_name('in-flight-updated-selection')
    pending = scope.with_name('in-flight-pending-selection')
    definition = prepare(scope, archive_dir, journal, runs, host)
    selector = scope / SELECTOR
    selector.write_text('two\n', encoding='utf-8')
    emitter = scope / 'emit-header.sh'
    script = emitter.read_text(encoding='utf-8')
    anchor = 'esac\n'
    assert script.count(anchor) == 1, script
    edit = f'''\
if [ "$score" = 17 ] && [ ! -e {shlex.quote(str(sentinel))} ]; then
    printf '%s\\n' two > {shlex.quote(str(pending))} || exit 1
    /bin/mv -f {shlex.quote(str(pending))} {shlex.quote(str(selector))} || exit 1
    : > {shlex.quote(str(sentinel))} || exit 1
fi
'''
    emitter.write_text(script.replace(anchor, anchor + edit), encoding='utf-8')

    _, events = build(ycxx, scope, env, other, other, 'warm receipts for the replacement archive')
    check_external_actions(events, 'z_provider', 'a_provider', 'scan_config')
    assert not sentinel.exists()
    check_binary(scope, other, other, 23, host)
    installed = published(scope, other)

    replace_source(scope, SELECTOR, 'one\n')
    _, events = build(ycxx, scope, env, other, other, 'discard a completed effect from an obsolete host archive')
    check_external_actions(events, 'z_provider', 'scan_config')
    assert not RAN & tu_outcomes(events, SCANNER), events
    assert sentinel.exists() and selector.read_text(encoding='utf-8') == 'two\n'
    assert (scope / 'build.ycxx').read_text(encoding='utf-8') == definition
    assert journal_contents(runs) == f'{host} {other} two\n{host} {other} one\n'
    assert journal_contents(journal) == '23\n17\n'
    check_binary(scope, other, other, 23, host)
    assert published(scope, other) == installed

    replace_source(scope, SELECTOR, 'one\n')
    _, events = build(ycxx, scope, env, other, other, 'run the previously rejected scanner effect once it is current')
    check_external_actions(events, 'scan_config')
    assert journal_contents(journal) == '23\n17\n17\n'
    check_binary(scope, other, other, 17, host)


def check_early(ycxx, scope, env, archive_dir, host, other, *, witness):
    journal, runs = (scope.with_name(f'{scope.name}-{name}-runs') for name in ('inventory', 'archive'))
    if witness:
        prepare_witness(scope)
    else:
        prepare_planning(scope, journal)
    manifest = scope / 'build.ycxx'
    attach_archives(scope, archive_dir, runs, manifest.read_text(encoding='utf-8'), host)

    for status in (17, 23):
        if status == 23:
            replace_source(scope, SELECTOR, 'two\n')
            stale, _ = reports(ycxx, scope, env, other, journal, runs)
            assert stale['dependency_graph']['complete'] is False, stale
        _, events = build(ycxx, scope, env, other, other, 'stage ordered archives before an early rule completes')
        assert 'executed' in link_outcomes(events, 'scanner'), events
        assert 'z_provider' in action_keys(events, 'ExternalAction'), events
        if status == 17:
            assert 'executed' in tu_outcomes(events, SCANNER), events
        else:
            assert not RAN & tu_outcomes(events, SCANNER), events
        if witness:
            check_observation(events)
        else:
            assert journal_contents(journal) == ('17\n' if status == 17 else '17\n23\n')
        check_binary(scope, other, other, status if witness else {17: 7, 23: 3}[status], host)
        plan, report = reports(ycxx, scope, env, other, journal, runs)
        graph = plan['dependency_graph']
        assert graph['complete'], graph
        assert all(graph['edges'].count(edge) == 1 for edge in archive_edges(plan, ORDER)), graph
        if witness:
            assert f'witness exit {status}' in fact(plan, 'value-export', 'observed', score_value(status))['origin']
            check_value_bindings_html(report, plan)
        else:
            receipt = fact(plan, 'plan-input', 'retry_policy', str(namespace(other) / 'virtual/inventory/inventory.txt'))
            assert receipt['origin'].startswith(INVENTORY), receipt
        check_dependency_html(report, plan)
        check_target_dependencies_html(report, plan)


def check_rejection(ycxx, scope, env, archive_dir, host, other, *, cycle):
    journal, runs = (scope.with_name(f'{scope.name}-{name}-runs') for name in ('scanner', 'archive'))
    definition = prepare(scope, archive_dir, journal, runs, host)
    field = 'selector = "archive-selection.txt"'
    assert definition.count(field) == 1, definition
    if cycle:
        reference = REFERENCE
        diagnostic = 'host executable generated archive dependency cycle'
    else:
        reference = 'artifact("other_tool", "executable")'
        definition += '\ncpp.host_executable "other_tool" { sources = ["other.cc"] }\n'
        (scope / 'other.cc').write_text('int main() { return 0; }\n', encoding='utf-8')
        diagnostic = 'generated archives for host executables require a producer chain without host executable inputs'
    (scope / 'build.ycxx').write_text(definition.replace(field, f'{field} helper = {reference}'), encoding='utf-8')
    summary, events = traced_build(ycxx, scope, env, 'reject a native host archive producer cycle or dependency',
                                   *target_args(other), succeeds=False)
    assert any(diagnostic in item for item in summary['diagnostics']), summary
    check_external_actions(events)
    assert not action_keys(events, 'Link') and not any(event.get('kind') == 'Tu' for event in events), events
    assert not any(path.exists() for path in (journal, runs, scope / executable(other)))


def main():
    ycxx, archiver = (Path(arg).resolve() for arg in sys.argv[1:3])
    host, other = {'linux': (LINUX, DARWIN), 'darwin': (DARWIN, LINUX)}[sys.platform]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-host-static-archive-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        env = env_for(runtime)
        archive_dir = prepare_archives(ycxx, archiver, root, env, host)
        checks = (
            ('project', check_builds, {}),
            ('in-flight', check_in_flight, {}),
            ('witness', check_early, {'witness': True}),
            ('planning', check_early, {'witness': False}),
            ('cycle', check_rejection, {'cycle': True}),
            ('host-producer', check_rejection, {'cycle': False}),
        )
        for name, check, options in checks:
            scope = root / name
            try:
                check(ycxx, scope, env, archive_dir, host, other, **options)
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
