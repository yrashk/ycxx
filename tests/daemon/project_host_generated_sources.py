"""Compile generated sources for native host tools during a foreign build (#1583)."""

import json
from pathlib import Path
import shlex
import sys
import tempfile

from project_build import action_keys, daemon_identity, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_host_generated_headers import (
    HOST_HEADER, SOURCE as SCHEMA_SOURCE, VIRTUAL as SCHEMA_DIR, attach_host_header,
)
from project_host_planning_inputs import COPY as INVENTORY, VERIFIER, prepare as prepare_planning, prepare_producers
from project_inspection import (
    check_dependency_graph, check_dependency_html, check_location, check_target_dependencies_html,
    check_value_bindings_html, dependency_edge, fact, scope_state,
)
from project_native_host_tool import GENERATED_HEADER, NOTE, REFERENCE, SCANNER, prepare as prepare_tool
from project_native_host_witness import prepare as prepare_witness
from project_shader_bundle import check_external_actions
from project_staged_witness import RULE, check_observation, check_observation_reuse, score_value
from project_targets import (
    DARWIN, LINUX, RAN, build, check_binary, executable, journal_contents, namespace, published, target_args,
)
from runtime_dir import runtime_root


DISK = 'disk.cc'
MAIN = 'generated_scanner.cc'
SIBLING = 'scanner_sibling.hh'
TEMPLATE = 'scanner.template'
SETTINGS = 'source-settings.txt'
SOURCE = 'artifact("scanner_source", "source")'
VIRTUAL = '.ycxx/host/virtual/scanner_source'
BONUS = 'include/bonus.hh'
SOURCE_TEMPLATE = f'''\
#include "{SIBLING}"
#include "scanner_schema.hh"
#if __has_include(<bonus.hh>)
#include <bonus.hh>
#else
inline constexpr int disk_extra = 0;
#endif
int disk_bonus();
int main() {{ return schema_score + sibling_bonus + SOURCE_BONUS + disk_bonus() + disk_extra; }}
'''
SOURCE_EMITTER = r'''#!/bin/sh
IFS=' ' read -r bonus note < "$2" || exit 1
printf '%s\n' "$bonus" >> "$3" || exit 1
printf '#define SOURCE_BONUS %s\n' "$bonus" > "$4" || exit 1
/bin/cat "$1" >> "$4" || exit 1
printf 'inline constexpr int sibling_bonus = 2;\n' > "$5" || exit 1
printf '%s\n' "$note" > "$6"
'''


def prepare_mixed(scope, journal, schemas, sources):
    definition = attach_host_header(scope, schemas, prepare_tool(scope, journal))
    field = f'sources = ["{SCANNER}"]'
    assert definition.count(field) == 1, definition
    definition = definition.replace(field, f'sources = ["{DISK}", {SOURCE}]\n    public_include_dirs = ["include"]')
    (scope / 'build.ycxx').write_text(definition + f'''\

generate "scanner_source" {{
    tool = "/bin/sh"
    args = [input("script"), input("template"), input("settings"), {json.dumps(str(sources))},
            output("source"), output("sibling"), output("detail")]
    inputs = {{ script = "emit-source.sh" template = "{TEMPLATE}" settings = "{SETTINGS}" }}
    outputs = {{ source = "{MAIN}" sibling = "{SIBLING}" detail = "source-detail.txt" }}
}}
''', encoding='utf-8')
    (scope / DISK).write_text('''\
#include "scanner_schema.hh"
static_assert(schema_score == 17);
#if __has_include("scanner_sibling.hh")
#error the generated source directory leaked into disk source search
#endif
int disk_bonus() { return -2; }
''', encoding='utf-8')
    (scope / 'include').mkdir()
    (scope / TEMPLATE).write_text(SOURCE_TEMPLATE, encoding='utf-8')
    (scope / SETTINGS).write_text('0 original evidence\n', encoding='utf-8')
    (scope / 'emit-source.sh').write_text(SOURCE_EMITTER, encoding='utf-8')
    emitter = scope / 'emit-header.sh'
    spelling = emitter.read_text(encoding='utf-8')
    assert spelling.count('17|23)') == 1, spelling
    emitter.write_text(spelling.replace('17|23)', '17|23|29)'), encoding='utf-8')


def reports(ycxx, scope, env, other, *journals):
    before = scope_state(scope)
    records = [journal_contents(journal) for journal in journals]
    args = ('show', 'app', *target_args(other))
    data = run_client(ycxx, scope, env, *args, '--format=json')
    report = run_client(ycxx, scope, env, *args, '--format=html')
    assert data.returncode == report.returncode == 0, (data.stderr, report.stderr)
    assert scope_state(scope) == before, 'inspection changed the workspace'
    assert [journal_contents(journal) for journal in journals] == records, 'inspection ran a tool'
    return json.loads(data.stdout), report.stdout


def check_plan(ycxx, scope, env, host, other, *journals):
    plan, report = reports(ycxx, scope, env, other, *journals)
    definition = plan['definition']
    target, = (row for row in plan['facts'] if row['kind'] == 'native-target')
    assert (target['target'], target['name'], target['value']) == ('app', 'triple', other), target
    assert fact(plan, 'host-target', 'scanner', host)['name'] == 'triple'
    source = fact(plan, 'source', 'scanner', f'{VIRTUAL}/{MAIN}')
    assert source['origin'] == SOURCE, source
    check_location(source, definition, SOURCE)
    fact(plan, 'source', 'scanner', DISK)
    include = fact(plan, 'include', 'scanner', SCHEMA_DIR)
    assert include['origin'] == HOST_HEADER, include
    assert not any(row['kind'] == 'include' and row['value'] == VIRTUAL for row in plan['facts']), plan
    check_dependency_graph(plan, [
        dependency_edge(definition, 'app', 'scan_config', 'generated_headers', GENERATED_HEADER, output='header'),
        dependency_edge(definition, 'app', 'scan_config', 'runtime_assets', NOTE, output='note'),
        dependency_edge(definition, 'scan_config', 'scanner', 'inputs', REFERENCE,
                        output='executable', binding='scanner'),
        dependency_edge(definition, 'scanner', 'scanner_source', 'sources', SOURCE, output='source'),
        dependency_edge(definition, 'scanner', 'scanner_schema', 'generated_headers', HOST_HEADER, output='header'),
        dependency_edge(definition, 'scanner_schema', 'schema_source', 'inputs', SCHEMA_SOURCE,
                        output='header', binding='header'),
    ])
    check_dependency_html(report, plan)
    check_target_dependencies_html(report, plan)


def check_mixed(ycxx, scope, env, host, other):
    journal, schemas, sources = (scope.with_name(f'mixed-{name}') for name in ('scanner', 'schemas', 'sources'))
    prepare_mixed(scope, journal, schemas, sources)
    plan, _ = reports(ycxx, scope, env, other, journal, schemas, sources)
    check_dependency_graph(plan, [], complete=False)
    assert not (scope / '.ycxx').exists(), 'cold inspection created build state'

    _, events = build(ycxx, scope, env, other, other, 'build selected generated and disk host translation units')
    check_external_actions(events, 'scanner_source', 'schema_source', 'scanner_schema', 'scan_config')
    assert all('executed' in tu_outcomes(events, name) for name in (MAIN, DISK, 'main.cc')), events
    check_binary(scope, other, other, 17, host)
    assert journal_contents(journal) == '17\n' and journal_contents(sources) == '0\n'
    check_plan(ycxx, scope, env, host, other, journal, schemas, sources)

    installed = published(scope, other)
    replace_source(scope, SETTINGS, '  0 original evidence  \n')
    _, events = build(ycxx, scope, env, other, other, 'reuse the host image with equal selected sources and siblings')
    check_external_actions(events, 'scanner_source')
    assert not any(RAN & tu_outcomes(events, name) for name in (MAIN, DISK, 'main.cc')), events
    assert published(scope, other) == installed and journal_contents(journal) == '17\n'
    assert journal_contents(sources) == '0\n0\n'

    virtual = scope / VIRTUAL
    virtual.mkdir(parents=True, exist_ok=True)
    for name in (MAIN, SIBLING, 'disk_only.hh'):
        (virtual / name).write_text('#error disk bytes reached a virtual input\n', encoding='utf-8')
    guard = '#if __has_include("disk_only.hh")\n#error virtual source lookup reached disk\n#endif\n'
    replace_source(scope, TEMPLATE, guard + SOURCE_TEMPLATE)
    replace_source(scope, SETTINGS, '6 changed emitted main\n')
    _, events = build(ycxx, scope, env, other, other, 'compile changed generated main from its private virtual directory')
    check_external_actions(events, 'scanner_source', 'scan_config')
    assert 'executed' in tu_outcomes(events, MAIN), events
    check_binary(scope, other, other, 23, host)
    assert journal_contents(journal) == '17\n23\n'

    watch = Watch(ycxx, scope, env, *target_args(other))
    try:
        _, events, watched = watch.build('record the real include miss for a generated main')
        check_external_actions(events)
        assert {str(scope / path) for path in (TEMPLATE, SETTINGS, DISK)} <= set(watched), watched
        assert not (scope / BONUS).exists()
        replace_source(scope, BONUS, 'inline constexpr int disk_extra = 6;\n')
        _, events, watched = watch.build('resolve the disk include miss without changing the generated main')
        assert str(scope / BONUS) in watched, watched
        check_external_actions(events, 'scan_config')
        assert 'executed' in tu_outcomes(events, MAIN), events
        check_binary(scope, other, other, 29, host)
        assert journal_contents(journal) == '17\n23\n29\n'
        assert journal_contents(sources) == '0\n0\n6\n'
        check_plan(ycxx, scope, env, host, other, journal, schemas, sources)

        installed = published(scope, other)
        replace_source(scope, TEMPLATE, '#error rejected generated scanner\n')
        summary, events, _ = watch.build('diagnose the generated source and preserve the last foreign app', succeeds=False)
        diagnostic = f'{VIRTUAL}/{MAIN}:2:2: rejected generated scanner'
        assert any(diagnostic in message for message in summary['diagnostics']), summary
        check_external_actions(events, 'scanner_source')
        assert published(scope, other) == installed
        replace_source(scope, TEMPLATE, guard + SOURCE_TEMPLATE)
        _, events, _ = watch.build('restore the generated source and reuse its successful image')
        check_external_actions(events)
        assert published(scope, other) == installed and journal_contents(journal) == '17\n23\n29\n'
        watch.idle('watch after generated source repair')
    finally:
        watch.close()


def check_in_flight(ycxx, scope, env, host, other):
    journal, schemas, sources = (scope.with_name(f'in-flight-{name}') for name in ('scanner', 'schemas', 'sources'))
    pending = scope.with_name('new-source-settings')
    sentinel = scope.with_name('source-settings-edited')
    seed = '  0 original evidence  \n'
    prepare_mixed(scope, journal, schemas, sources)
    emitter = scope / 'emit-header.sh'
    script = emitter.read_text(encoding='utf-8')
    anchor = 'esac\n'
    assert script.count(anchor) == 1
    edit = f'''\
if [ ! -e {shlex.quote(str(sentinel))} ]; then
    printf '%s' {shlex.quote(seed)} > {shlex.quote(str(pending))} || exit 1
    /bin/mv -f {shlex.quote(str(pending))} {shlex.quote(str(scope / SETTINGS))} || exit 1
    : > {shlex.quote(str(sentinel))} || exit 1
fi
'''
    emitter.write_text(script.replace(anchor, anchor + edit), encoding='utf-8')

    _, events = build(ycxx, scope, env, other, other, 'admit a completed scanner effect across equal source restaging')
    assert sentinel.exists() and (scope / SETTINGS).read_text(encoding='utf-8') == seed
    assert journal_contents(sources) == '0\n0\n'
    assert journal_contents(journal) == '17\n'
    check_external_actions(events, 'scanner_source', 'scanner_source', 'schema_source', 'scanner_schema', 'scan_config')
    check_binary(scope, other, other, 17, host)


def make_generated_only(scope, sources):
    manifest = scope / 'build.ycxx'
    definition = manifest.read_text(encoding='utf-8')
    for name, source, status in sources:
        disk = f'sources = ["{source}"]'
        assert definition.count(disk) == 1, definition
        definition = definition.replace(disk, f'sources = [artifact("{name}_source", "source")]')
        (scope / source).unlink()
        seed = f'{name}.template'
        (scope / seed).write_text(f'int main() {{ return {status}; }}\n', encoding='utf-8')
        definition += f'''\

generate "{name}_source" {{
    tool = "/bin/cp"
    args = [input("source"), output("source")]
    inputs = {{ source = "{seed}" }}
    outputs = {{ source = "{source}" }}
}}
'''
    manifest.write_text(definition, encoding='utf-8')


def check_independent_producer_guard(ycxx, scope, env, other):
    journal, schemas, sources = (scope.with_name(f'rejected-{name}') for name in ('scanner', 'schemas', 'sources'))
    prepare_mixed(scope, journal, schemas, sources)
    manifest = scope / 'build.ycxx'
    definition = manifest.read_text(encoding='utf-8')
    field = f'settings = "{SETTINGS}"'
    assert definition.count(field) == 1
    definition = definition.replace(field, f'{field} helper = artifact("other_tool", "executable")')
    definition += '\ncpp.host_executable "other_tool" { sources = ["other.cc"] }\n'
    manifest.write_text(definition, encoding='utf-8')
    (scope / 'other.cc').write_text('int main() { return 0; }\n', encoding='utf-8')

    summary, events = traced_build(ycxx, scope, env, 'reject another host executable in the source producer chain',
                                   *target_args(other), succeeds=False)
    diagnostic = 'generated sources for host executables require a producer chain without host executable inputs'
    assert any(diagnostic in item for item in summary['diagnostics']), summary
    check_external_actions(events)
    assert not action_keys(events, 'Link') and not RAN & tu_outcomes(events, 'other.cc'), events
    assert not any(path.exists() for path in (journal, sources, schemas, scope / executable(other)))


def check_early(ycxx, scope, env, host, other, *, witness):
    inventory_runs = scope.with_name('inventory-runs')
    if witness:
        prepare_witness(scope)
        make_generated_only(scope, [('scanner', SCANNER, 17)])
    else:
        prepare_planning(scope, inventory_runs)
        make_generated_only(scope, [('scanner', SCANNER, 17), ('verifier', VERIFIER, 9)])

    # These host mains read no workspace headers and have no disk translation units.
    _, events = build(ycxx, scope, env, other, other, 'compile generated-only host tools before completing the plan')
    assert 'executed' in tu_outcomes(events, SCANNER), events
    if witness:
        check_observation(events)
    else:
        assert journal_contents(inventory_runs) == '17\n'
    check_binary(scope, other, other, 17 if witness else 7, host)

    snapshot, = (scope / namespace(other) / 'rules').glob('target-*.json')
    stored = json.loads(snapshot.read_text(encoding='utf-8'))
    assert stored['target'] == 'app', snapshot
    name, evidence = ('observed', 'host_files') if witness else ('retry_policy', 'planning_host_files')
    record, = (row for row in stored['rules'] if row['target'] == name)
    assert record[evidence] == [], (name, evidence, record[evidence])

    def inspect_snapshot():
        plan, report = reports(ycxx, scope, env, other, inventory_runs)
        assert plan['dependency_graph']['complete'], plan
        assert fact(plan, 'host-target', 'scanner', host)['name'] == 'triple'
        assert fact(plan, 'source', 'scanner', f'{VIRTUAL}/{SCANNER}')['origin'] == SOURCE
        edge = dependency_edge(plan['definition'], 'scanner', 'scanner_source', 'sources', SOURCE, output='source')
        assert edge in plan['dependency_graph']['edges'], plan
        if witness:
            assert 'witness exit 17' in fact(plan, 'value-export', 'observed', score_value(17))['origin']
            check_value_bindings_html(report, plan)
        else:
            assert fact(plan, 'plan-input', 'retry_policy',
                        str(namespace(other) / 'virtual/inventory/inventory.txt'))['origin'].startswith(INVENTORY)
        check_dependency_html(report, plan)
        check_target_dependencies_html(report, plan)
        return plan

    saved = inspect_snapshot()
    previous = daemon_identity(ycxx, scope, env)
    assert previous, 'project build did not use a resident daemon'
    stopped = run_client(ycxx, scope, env, 'ctl', 'shutdown')
    assert stopped.returncode == 0, stopped.stderr
    assert inspect_snapshot() == saved, 'persisted snapshot lost the generated-only tool evidence'
    assert daemon_identity(ycxx, scope, env) is None, 'read-only show started a daemon'
    build(ycxx, scope, env, other, other, 'restart the daemon and verify the generated-only build')
    restarted = daemon_identity(ycxx, scope, env)
    assert restarted and restarted != previous, (previous, restarted)
    check_binary(scope, other, other, 17 if witness else 7, host)
    assert inspect_snapshot() == saved


def check_failed_witness_producer(ycxx, scope, env, other):
    inventory_runs = scope.with_name('combined-inventory-runs')
    failures = scope.with_name('combined-source-failures')
    prepare_witness(scope)
    make_generated_only(scope, [('scanner', SCANNER, 17)])
    producers = prepare_producers(scope, inventory_runs)
    producers = producers.replace('cpp.host_executable "scanner"', 'cpp.host_executable "inventory_scanner"')
    producers = producers.replace(REFERENCE, 'artifact("inventory_scanner", "executable")')

    manifest = scope / 'build.ycxx'
    definition = manifest.read_text(encoding='utf-8')
    observer = 'tools.observe "observed" {}'
    assert definition.count(observer) == 1
    definition = definition.replace(observer, f'tools.observe "observed" {{ plan_inputs = {{ inventory = {INVENTORY} }} }}')
    copy = 'tool = "/bin/cp"\n    args = [input("source"), output("source")]'
    assert definition.count(copy) == 1
    failed = 'generated witness main deliberately failed'
    script = f'''\
printf '%s\\n' failure >> {shlex.quote(str(failures))} || exit 1
printf '%s\\n' {shlex.quote(failed)} >&2
exit 1
'''
    definition = definition.replace(copy, f'tool = "/bin/sh"\n    args = ["-c", {json.dumps(script)}]')
    manifest.write_text(definition + producers, encoding='utf-8')

    rule = scope / RULE
    source = rule.read_text(encoding='utf-8')
    emit = '    return host->emit(host->context, observe, sizeof(observe) - 1);'
    assert source.count(emit) == 1
    rule.write_text(source.replace(emit, r'''    {
      ycxx_rule_bytes_v1 inventory{};
      if (!host->read || host->read(host->context, "inventory", &inventory) != 1 ||
          inventory.size != 2 || inventory.data[0] != '7' || inventory.data[1] != '\n')
        return reject(host, "expected confirmed native inventory");
      return host->emit(host->context, observe, sizeof(observe) - 1);
    }'''), encoding='utf-8')

    summary, events = traced_build(ycxx, scope, env, 'surface a current witness source producer failure',
                                   *target_args(other), succeeds=False)
    assert any(failed in item for item in summary['diagnostics']), summary
    assert action_keys(events, 'RuleWorker').count('observed') == 1, events
    check_external_actions(events, 'inventory_source', 'inventory', failed=('scanner_source',))
    check_observation_reuse(events)
    assert journal_contents(inventory_runs) == '17\n'
    assert journal_contents(failures) == 'failure\n'
    assert not (scope / executable(other)).exists()


def main():
    ycxx = Path(sys.argv[1]).resolve()
    host, other = {'linux': (LINUX, DARWIN), 'darwin': (DARWIN, LINUX)}[sys.platform]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-host-generated-sources-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        env = env_for(runtime)
        for name, check in (('mixed', check_mixed), ('in-flight', check_in_flight)):
            scope = root / name
            try:
                check(ycxx, scope, env, host, other)
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')
        for mode in ('witness', 'planning'):
            scope = root / mode
            try:
                check_early(ycxx, scope, env, host, other, witness=mode == 'witness')
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')
        for name, check in (('rejected', check_independent_producer_guard), ('combined', check_failed_witness_producer)):
            scope = root / name
            try:
                check(ycxx, scope, env, other)
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
