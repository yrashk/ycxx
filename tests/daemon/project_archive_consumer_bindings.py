"""Select role-specific static archive outputs from one shared library (#1629)."""

from collections import Counter
import json
from pathlib import Path
import struct
import sys
import tempfile

from project_build import action_keys, check_program, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_host_generated_sources import reports
from project_host_library_static_archive_inputs import EMPTY_ARCHIVE
from project_host_static_archive_inputs import HOST_VIRTUAL, link_outcomes
from project_inspection import (
    check_dependency_graph, check_dependency_html, check_target_dependencies_html, check_target_graph_html,
    dependency_edge, fact,
)
from project_link_inputs import checked_command
from project_native_host_tool import GENERATED_HEADER, NOTE, REFERENCE, SCANNER, prepare as prepare_tool
from project_shader_bundle import check_external_actions
from project_targets import (
    DARWIN, LINUX, RAN, build, check_binary, executable, journal_contents, namespace, published, target_args,
)
from runtime_dir import runtime_root


HOST_DATA = 'host-input.a'
TARGET_DATA = 'target-input.a'
FINAL_UNITS = (SCANNER, 'shared.cc', 'main.cc')
SEPARATE = (('archive_host', 'archive', 'host.a'), ('archive_target', 'archive', 'target.a'))
PAIRED = (('archive_pair', 'host_archive', 'host.a'), ('archive_pair', 'target_archive', 'target.a'))
PLAIN = ('archive_plain', 'archive', 'plain.a')
DIRECT = ('archive_direct', 'archive', 'direct.a')
EMIT = r'''
producer=$1
journal=$2
shift 2
while [ "$#" -gt 0 ]; do
    /bin/cp "$1" "$2" || exit 1
    shift 2
done
printf '%s\n' "$producer" >> "$journal"
'''


def member(name, data):
    header = f'{name:16}{0:<12}{0:<6}{0:<6}{644:<8}{len(data):<10}`\n'
    assert len(header) == 60
    return header.encode('ascii') + data + (b'\n' if len(data) % 2 else b'')


def pack_archive(triple, code):
    # One native object with a single indexed symbol in either GNU or BSD archive format.
    if triple == LINUX:
        index_name, object_name, symbol = '/', 'score.o/', b'linked_score\0'
        placeholder = struct.pack('>II', 1, 0) + symbol
        offset = len(EMPTY_ARCHIVE) + len(member(index_name, placeholder))
        index = struct.pack('>II', 1, offset) + symbol
    else:
        index_name, object_name, symbol = '__.SYMDEF', 'score.o', b'_linked_score\0'
        strings = symbol + b'\0' * (-len(symbol) % 8)
        placeholder = struct.pack('<IIII', 8, 0, 0, len(strings)) + strings
        offset = len(EMPTY_ARCHIVE) + len(member(index_name, placeholder))
        index = struct.pack('<IIII', 8, 0, offset, len(strings)) + strings
    return EMPTY_ARCHIVE + member(index_name, index) + member(object_name, code)


def prepare_archives(ycxx, root, env, host, other):
    directory = root / 'archive-objects'
    directory.mkdir()
    standalone = dict(env, YCXX_NO_DAEMON='1')
    standalone.pop('YCXX_DAEMON', None)
    archives = {}
    for triple, scores in ((host, (17, 23, 29, 31)), (other, (29, 41))):
        for score in scores:
            (directory / 'score.cc').write_text(f'extern "C" int linked_score() {{ return {score}; }}\n', encoding='utf-8')
            checked_command(ycxx, 'cxx', f'--target={triple}', '-c', 'score.cc', '-o', 'score.o',
                            cwd=directory, env=standalone)
            code = (directory / 'score.o').read_bytes()
            assert code[:4] == (b'\x7fELF' if triple == LINUX else bytes.fromhex('cffaedfe'))
            archives[triple, score] = pack_archive(triple, code)
    return archives


def reference(archive):
    producer, output, _ = archive
    return f'artifact("{producer}", "{output}")'


def role_binding(archives):
    host, target = (reference(archive) for archive in archives)
    return f'{{ host = {host}, target = {target} }}'


def generator(journal, name, bindings):
    words = ', '.join(f'input("{output}"), output("{output}")' for _, output, _ in bindings)
    inputs = ' '.join(f'{output} = "{path}"' for path, output, _ in bindings)
    outputs = ' '.join(f'{output} = "{filename}"' for _, output, filename in bindings)
    return f'''\

generate "{name}" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(EMIT)}, "emit-archives", "{name}", {json.dumps(str(journal))}, {words}]
    inputs = {{ {inputs} }}
    outputs = {{ {outputs} }}
}}
'''


def prepare(scope, archives, scanner_runs, archive_runs, host, selected, *, paired=False, traversal='both'):
    definition = prepare_tool(scope, scanner_runs)
    roles = PAIRED if paired else SEPARATE
    if traversal != 'target':
        source = f'sources = ["{SCANNER}"]'
        assert definition.count(source) == 1, definition
        definition = definition.replace(source, f'{source}\n    deps = ["shared"]')
    app = 'sources = ["main.cc"]'
    assert definition.count(app) == 1, definition
    if traversal != 'host':
        definition = definition.replace(app, f'{app}\n    deps = ["shared"]')
    definition = definition.replace(f'    generated_headers = [{GENERATED_HEADER}]\n', '')
    if traversal == 'target':
        definition = definition.replace(f'    runtime_assets = [{NOTE}]\n', '')

    variant = role_binding(roles)
    operands = [variant, variant, reference(PLAIN)] if paired else [variant]
    body = ',\n'.join(f'        {operand}' for operand in operands)
    definition += f'''\

cpp.library "shared" {{
    sources = ["shared.cc"]
    public_link_inputs = [
{body}
    ]
}}
'''
    if paired:
        definition += generator(archive_runs, 'archive_pair', [
            (HOST_DATA, *roles[0][1:]), (TARGET_DATA, *roles[1][1:]),
        ])
        definition += generator(archive_runs, PLAIN[0], [('plain-input.a', *PLAIN[1:])])
        definition += generator(archive_runs, DIRECT[0], [('direct-input.a', *DIRECT[1:])])
        (scope / 'plain-input.a').write_bytes(EMPTY_ARCHIVE)
        (scope / 'direct-input.a').write_bytes(archives[host, 31])
    else:
        for archive, path in zip(roles, (HOST_DATA, TARGET_DATA)):
            name, output, filename = archive
            definition += generator(archive_runs, name, [(path, output, filename)])
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    (scope / HOST_DATA).write_bytes(archives[host, 17])
    (scope / TARGET_DATA).write_bytes(archives[selected, 29])

    macro = {LINUX: '__x86_64__', DARWIN: '__aarch64__'}[host]
    (scope / SCANNER).write_text(f'''\
#if !defined({macro})
#error scanner must be compiled for the process host
#endif
int shared_score();
int main() {{ return shared_score(); }}
''', encoding='utf-8')
    (scope / 'shared.cc').write_text('extern "C" int linked_score(); int shared_score() { return linked_score(); }\n',
                                     encoding='utf-8')
    source = 'int main() { return 0; }\n' if traversal == 'host' else (
        'int shared_score(); int main() { return shared_score(); }\n')
    (scope / 'main.cc').write_text(source, encoding='utf-8')
    emitter = scope / 'emit-header.sh'
    script = emitter.read_text(encoding='utf-8')
    assert script.count('17|23)') == 1, script
    emitter.write_text(script.replace('17|23)', '17|23|31)'), encoding='utf-8')
    return definition, roles


def archive_edges(plan, root, directory, role, archive, *, paired, direct=False):
    definition = plan['definition']
    rows = sorted((row for row in plan['facts'] if row['kind'] == 'link-input' and row['target'] == root),
                  key=lambda row: int(row['name']))
    uses = [(role, archive)]
    if paired:
        uses += [(role, archive), (None, PLAIN)]
    if direct:
        uses.insert(0, (None, DIRECT))
    assert len(rows) == len(uses), (root, rows, uses)
    start, edges = definition.index('public_link_inputs = ['), []
    for index, (row, (binding, item)) in enumerate(zip(rows, uses)):
        producer, output, filename = item
        spelling = reference(item)
        if direct and index == 0:
            owner, usage = root, 'link_inputs'
            site = definition.index('link_inputs = [')
        else:
            owner, usage = 'shared', 'public_link_inputs'
            site = start
            start = definition.index(spelling, start) + len(spelling)
        edge = dependency_edge(definition, owner, producer, usage, spelling, start=site,
                               binding=binding, output=output)
        location = edge['location']
        origin = f'declared in {usage} as {spelling}'
        if binding:
            origin = f'declared in {usage} as {binding} = {spelling}'
        if owner != root:
            origin = f'inherited from {owner} ({origin})'
        assert (row['name'], row['value'], row['origin']) == (str(index), str(directory / producer / filename), origin), row
        assert (row['line'], row['column']) == (location['line'], location['column']), row
        edges.append(edge)
    return edges


def inspect(ycxx, scope, env, host, selected, journals, roles, *, paired=False, traversal='both', direct=False):
    plan, report = reports(ycxx, scope, env, selected, *journals)
    definition = plan['definition']
    target, = (row for row in plan['facts'] if row['kind'] == 'native-target')
    assert (target['target'], target['value']) == ('app', selected or host), target
    edges = []
    if traversal != 'target':
        assert fact(plan, 'host-target', 'scanner', host)['name'] == 'triple'
        edges = [
            dependency_edge(definition, 'app', 'scan_config', 'runtime_assets', NOTE, output='note'),
            dependency_edge(definition, 'scan_config', 'scanner', 'inputs', REFERENCE,
                            binding='scanner', output='executable'),
            dependency_edge(definition, 'scanner', 'shared', 'deps', '"shared"',
                            start=definition.index('cpp.host_executable "scanner"')),
            *archive_edges(plan, 'scanner', HOST_VIRTUAL, 'host', roles[0], paired=paired, direct=direct),
        ]
    if traversal != 'host':
        edges.append(dependency_edge(definition, 'app', 'shared', 'deps', '"shared"',
                                     start=definition.index('cpp.executable "app"')))
        for edge in archive_edges(plan, 'app', namespace(selected) / 'virtual', 'target', roles[1], paired=paired):
            if edge not in edges:
                edges.append(edge)
    check_dependency_graph(plan, edges)
    check_dependency_html(report, plan)
    check_target_dependencies_html(report, plan)
    check_target_graph_html(report, plan)


def archive_edit(scope, name, contents):
    pending = scope / '.ycxx' / 'archive-fixture-update'
    pending.write_bytes(contents)
    pending.replace(scope / name)


def check_warm_units(events):
    outcomes = {unit: tu_outcomes(events, unit) for unit in FINAL_UNITS}
    assert not any(RAN & states for states in outcomes.values()), outcomes


def check_watch(ycxx, scope, env, archives, host, other, scanner_runs):
    watch = Watch(ycxx, scope, env, *target_args(other))
    try:
        summary, events, watched = watch.build('watch the separate host and selected archive operands')
        assert summary['actions_executed'] == 0, summary
        assert {str(scope / name) for name in (HOST_DATA, TARGET_DATA, *FINAL_UNITS)} <= set(watched), watched
        check_external_actions(events)
        check_warm_units(events)
        initial = published(scope, other)

        archive_edit(scope, TARGET_DATA, archives[other, 41])
        _, events, _ = watch.build('change only the selected archive and relink only the app')
        check_external_actions(events, SEPARATE[1][0])
        assert action_keys(events, 'Link') == ['app'], events
        check_warm_units(events)
        target_updated = published(scope, other)
        assert target_updated[0][0] != initial[0][0] and target_updated[1] == initial[1]
        check_binary(scope, other, other, 17, host)

        archive_edit(scope, HOST_DATA, archives[host, 23])
        _, events, _ = watch.build('change only the host archive and relink only the scanner')
        check_external_actions(events, SEPARATE[0][0], 'scan_config')
        assert action_keys(events, 'Link') == ['scanner'], events
        check_warm_units(events)
        good = published(scope, other)
        assert good[0] == target_updated[0] and good[1][0] != target_updated[1][0]
        check_binary(scope, other, other, 23, host)
        assert journal_contents(scanner_runs) == '17\n23\n'

        for role, name, invalid, valid, selected_path in (
            (0, HOST_DATA, archives[other, 41], archives[host, 23], HOST_VIRTUAL),
            (1, TARGET_DATA, archives[host, 23], archives[other, 41], namespace(other) / 'virtual'),
        ):
            producer, _, filename = SEPARATE[role]
            archive_edit(scope, name, invalid)
            summary, events, _ = watch.build(f'reject the incompatible {name}', succeeds=False)
            check_external_actions(events, producer)
            check_warm_units(events)
            path = str(selected_path / producer / filename)
            assert any(path in message and ('host executable' in message) == (role == 0)
                       for message in summary['diagnostics']), summary
            assert published(scope, other) == good and journal_contents(scanner_runs) == '17\n23\n'

            archive_edit(scope, name, valid)
            _, events, _ = watch.build(f'recover after restoring {name}')
            check_warm_units(events)
            assert published(scope, other) == good
        watch.idle('watch after both archive bindings recover')
    finally:
        watch.close()


def check_cross(ycxx, scope, env, archives, host, other):
    journals = tuple(scope.with_name(f'cross-{name}-runs') for name in ('scanner', 'archive'))
    scanner_runs, archive_runs = journals
    _, roles = prepare(scope, archives, *journals, host, other)
    cold, _ = reports(ycxx, scope, env, other, *journals)
    check_dependency_graph(cold, [], complete=False)
    assert not (scope / '.ycxx').exists()
    _, events = build(ycxx, scope, env, other, other, 'choose distinct archives for host and selected target')
    check_external_actions(events, *[archive[0] for archive in roles], 'scan_config')
    assert action_keys(events, 'Link') == ['app', 'scanner'], events
    assert Counter(journal_contents(archive_runs).splitlines()) == Counter(archive[0] for archive in roles)
    assert journal_contents(scanner_runs) == '17\n'
    check_binary(scope, other, other, 17, host)
    inspect(ycxx, scope, env, host, other, journals, roles)
    check_watch(ycxx, scope, env, archives, host, other, scanner_runs)


def check_same_triple(ycxx, scope, env, archives, host):
    journals = tuple(scope.with_name(f'same-triple-{name}-runs') for name in ('scanner', 'archive'))
    scanner_runs, archive_runs = journals
    definition, roles = prepare(scope, archives, *journals, host, host, paired=True)
    _, events = build(ycxx, scope, env, host, host, 'use both role outputs even with an identical target triple')
    check_external_actions(events, PAIRED[0][0], PLAIN[0], 'scan_config')
    assert action_keys(events, 'Link') == ['app', 'scanner'], events
    check_program(scope / executable(host), 29)
    assert journal_contents(scanner_runs) == '17\n'
    assert Counter(journal_contents(archive_runs).splitlines()) == Counter([PAIRED[0][0], PLAIN[0]])
    inspect(ycxx, scope, env, host, host, journals, roles, paired=True)
    initial = published(scope, host)

    sources = f'sources = ["{SCANNER}"]'
    assert definition.count(sources) == 1, definition
    replace_source(scope, 'build.ycxx', definition.replace(sources, f'{sources}\n    link_inputs = [{reference(DIRECT)}]'))
    _, events = build(ycxx, scope, env, host, host, 'place the direct archive before the two repeated inherited roles')
    check_external_actions(events, DIRECT[0], 'scan_config')
    assert action_keys(events, 'Link') == ['scanner'], events
    check_warm_units(events)
    check_program(scope / executable(host), 29)
    assert journal_contents(scanner_runs) == '17\n31\n'
    assert published(scope, host)[0] == initial[0]
    inspect(ycxx, scope, env, host, host, journals, roles, paired=True, direct=True)


def check_single_traversal(ycxx, scope, env, archives, host, other, *, traversal):
    journals = tuple(scope.with_name(f'{traversal}-only-{name}-runs') for name in ('scanner', 'archive'))
    scanner_runs, archive_runs = journals
    native_app = traversal == 'target'
    target = host if native_app else other
    _, roles = prepare(scope, archives, *journals, host, target, traversal=traversal)
    inactive = HOST_DATA if native_app else TARGET_DATA
    (scope / inactive).write_bytes(b'an incorrectly staged inactive archive would fail the link')
    _, events = build(ycxx, scope, env, target, target, 'stage only the archive of the reached consumer role')
    role = roles[native_app][0]
    effects = (role,) if native_app else (role, 'scan_config')
    check_external_actions(events, *effects)
    assert journal_contents(archive_runs) == f'{role}\n'
    if native_app:
        check_program(scope / executable(target), 29)
        assert not scanner_runs.exists()
    else:
        check_binary(scope, other, other, 17, host)
        assert journal_contents(scanner_runs) == '17\n'
    inspect(ycxx, scope, env, host, target, journals, roles, traversal=traversal)


def check_rejection(ycxx, scope, env, archives, host, other, *, variant):
    journals = tuple(scope.with_name(f'{scope.name}-{name}-runs') for name in ('scanner', 'archive'))
    definition, roles = prepare(scope, archives, *journals, host, other)
    host_ref, target_ref = (reference(archive) for archive in roles)
    valid = role_binding(roles)
    if variant == 'missing':
        definition = definition.replace(valid, f'{{ host = {host_ref} }}')
        diagnostic = "public_link_inputs binding requires exactly 'host' and 'target' fields"
    elif variant == 'extra':
        definition = definition.replace(valid, f'{{ host = {host_ref}, target = {target_ref}, extra = {host_ref} }}')
        diagnostic = "public_link_inputs binding requires exactly 'host' and 'target' fields"
    elif variant == 'wrong-arm':
        definition = definition.replace(valid, f'{{ host = {host_ref}, target = "target.a" }}')
        diagnostic = "expected an artifact reference for 'target' in public_link_inputs"
    elif variant == 'direct':
        source = f'sources = ["{SCANNER}"]'
        definition = definition.replace(source, f'{source}\n    link_inputs = [{valid}]')
        diagnostic = 'expected an artifact reference'
    else:
        source = f'archive = "{HOST_DATA}"'
        assert definition.count(source) == 1, definition
        definition = definition.replace(source, f'{source} helper = artifact("second_tool", "executable")')
        definition += '\ncpp.host_executable "second_tool" { sources = ["second.cc"] }\n'
        (scope / 'second.cc').write_text('int main() { return 0; }\n', encoding='utf-8')
        diagnostic = 'generated archives for host executables require a producer chain without host executable inputs'
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')

    summary, events = traced_build(ycxx, scope, env, f'reject an invalid {variant} archive binding',
                                   *target_args(other), succeeds=False)
    assert any(diagnostic in message for message in summary['diagnostics']), summary
    check_external_actions(events)
    assert not any(event.get('kind') in {'Tu', 'Link'} for event in events), events
    assert not any(path.exists() for path in (*journals, scope / executable(other)))


def main():
    ycxx = Path(sys.argv[1]).resolve()
    host, other = {'linux': (LINUX, DARWIN), 'darwin': (DARWIN, LINUX)}[sys.platform]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-archive-consumer-binding-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        env = env_for(runtime)
        archives = prepare_archives(ycxx, root, env, host, other)
        for role, check in (('cross', check_cross), ('native', check_same_triple)):
            scope = root / role
            try:
                if role == 'cross':
                    check(ycxx, scope, env, archives, host, other)
                else:
                    check(ycxx, scope, env, archives, host)
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')
        for traversal in ('host', 'target'):
            scope = root / f'{traversal}-only'
            try:
                check_single_traversal(ycxx, scope, env, archives, host, other, traversal=traversal)
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')
        for variant in ('missing', 'extra', 'wrong-arm', 'direct', 'host-dependent'):
            scope = root / variant
            try:
                check_rejection(ycxx, scope, env, archives, host, other, variant=variant)
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
