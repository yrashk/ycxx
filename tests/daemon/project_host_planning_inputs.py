"""Read native-produced inventory through rule planning inputs (#1574)."""

import html
import json
from pathlib import Path
import re
import shlex
import shutil
import sys
import tempfile

from project_build import action_keys, check_program, env_for, run_client, tu_outcomes
from project_build_watch import Watch, replace_source
from project_inspection import (
    check_dependency_graph, check_dependency_html, check_location, check_target_dependencies_html,
    dependency_edge, fact, scope_state,
)
from project_native_host_tool import REFERENCE, SCANNER, SCORE_HEADER, STAGED, header
from project_package_probe import prepare as prepare_probe
from project_shader_bundle import check_external_actions
from project_targets import (
    DARWIN, LINUX, RAN, build, check_binary, check_target_trace, executable, journal_contents,
    namespace, published, target_args,
)
from runtime_dir import runtime_root


EXAMPLE = Path(__file__).resolve().parents[2] / 'examples/project-build/custom-rule'
COPY = 'artifact("inventory", "list")'
SOURCE = 'artifact("inventory_source", "list")'
VERIFIER = 'verifier.cc'
VERIFY = 'artifact("verifier", "executable")'
HEADER = 'artifact("retry_policy", "header")'
NOTE = 'artifact("retry_policy", "note")'
REPAIR = 'repair_score.hh'
EMITTER = r'''#!/bin/sh
"$2"
[ "$?" -eq 9 ] || exit 1
"$1"
status=$?
case "$status" in
    17|18) value=7 ;;
    19) value=' 7 ' ;;
    23) value=3 ;;
    *) printf 'unexpected scanner status %s\n' "$status" >&2; exit 1 ;;
esac
printf '%s\n' "$status" >> "$3" || exit 1
printf '%s\n' "$value" > "$4" || exit 1
printf '%s\n' "$status" > "$5"
'''


def prepare_producers(scope, journal):
    (scope / SCANNER).write_text(f'#include "{SCORE_HEADER}"\nint main() {{ return scanner_score; }}\n', encoding='utf-8')
    (scope / VERIFIER).write_text(f'''\
#include "{SCORE_HEADER}"
static_assert(scanner_score > 0);
int main() {{ return 9; }}
''', encoding='utf-8')
    (scope / SCORE_HEADER).write_text(header(17), encoding='utf-8')
    (scope / 'emit-inventory.sh').write_text(EMITTER, encoding='utf-8')
    return f'''\
cpp.host_executable "scanner" {{ sources = ["{SCANNER}"] }}
cpp.host_executable "verifier" {{ sources = ["{VERIFIER}"] }}

generate "inventory_source" {{
    tool = "/bin/sh"
    args = [input("emitter"), input("scanner"), input("verifier"), {json.dumps(str(journal))},
            output("list"), output("detail")]
    inputs = {{ emitter = "emit-inventory.sh" scanner = {REFERENCE} verifier = {VERIFY} }}
    outputs = {{ list = "inventory.txt" detail = "status.txt" }}
}}

generate "inventory" {{
    tool = "/bin/cp"
    args = [input("list"), output("list")]
    inputs = {{ list = {SOURCE} }}
    outputs = {{ list = "inventory.txt" }}
}}
'''


def prepare(scope, journal):
    shutil.copytree(EXAMPLE, scope, ignore=shutil.ignore_patterns('.ycxx'))
    producers = prepare_producers(scope, journal)
    manifest = scope / 'build.ycxx'
    definition = manifest.read_text(encoding='utf-8')
    reads = 'reads = ["retry-limit.txt"]'
    includes = f'generated_headers = [{HEADER}]'
    assert definition.count(reads) == definition.count(includes) == 1, definition
    definition = definition.replace(reads, f'plan_inputs = {{ inventory = {COPY} }}')
    definition = definition.replace(includes, f'{includes}\n    runtime_assets = [{NOTE}]')
    manifest.write_text(definition + '\n' + producers, encoding='utf-8')

    rule = scope / 'rules/retry_policy.cc'
    source = rule.read_text(encoding='utf-8')
    redirect = r' > \"$2\"",'
    assert source.count(redirect) == 1, source
    source = source.replace('retry-limit.txt', 'inventory').replace('be listed in reads', 'be listed in plan_inputs')
    source = source.replace(redirect, r''' > \"$2\" || exit 1; printf '%s\\n' \"$1\" > \"$3\"",''')
    source = source.replace('output("header")]', 'output("header"), output("note")]')
    source = source.replace('header = "retry_limit.hh"', 'header = "retry_limit.hh" note = "score-note.txt"')
    rule.write_text(source, encoding='utf-8')


def inspect(ycxx, scope, env, journal, selector, host, *, score=None, cold=False):
    files = scope_state(scope)
    logged = journal_contents(journal)
    arguments = ('show', 'app', *target_args(selector))
    result = run_client(ycxx, scope, env, *arguments, '--format=json')
    assert result.returncode == 0, result.stderr
    plan = json.loads(result.stdout)
    assert plan['schema'] == 1 and plan['target'] == 'app', plan
    canonical = selector or host
    target, = (row for row in plan['facts'] if row['kind'] == 'native-target')
    assert (target['target'], target['name'], target['value']) == ('app', 'triple', canonical), target
    shown = run_client(ycxx, scope, env, *arguments, '--format=html')
    assert shown.returncode == 0, shown.stderr
    receipt = None
    if score is None:
        unresolved = 'app' if cold else 'retry_policy'
        assert any((row['kind'], row['target']) == ('unresolved', unresolved) for row in plan['facts']), plan
        assert not any(row['kind'] == 'plan-input' for row in plan['facts']), plan
        assert plan['dependency_graph']['complete'] is False, plan
        if cold:
            check_dependency_graph(plan, [], complete=False)
            assert not (scope / '.ycxx').exists(), 'cold show created build state'
    else:
        definition = plan['definition']
        path = str(namespace(selector) / 'virtual/inventory/inventory.txt')
        item = fact(plan, 'plan-input', 'retry_policy', path)
        assert item['name'] == 'inventory', item
        check_location(item, definition, COPY)
        receipt = re.fullmatch(re.escape(f'{COPY} · receipt ') + r'([0-9a-f]{32})', item['origin'])
        assert receipt, item
        fact(plan, 'argv', 'retry_policy', str(score))
        for name in ('scanner', 'verifier'):
            assert fact(plan, 'host-target', name, host)['name'] == 'triple'
        assert fact(plan, 'input', 'inventory_source', STAGED.removeprefix('inputs/'))['origin'] == REFERENCE
        check_dependency_graph(plan, [
            dependency_edge(definition, 'app', 'retry_policy', 'generated_headers', HEADER, output='header'),
            dependency_edge(definition, 'app', 'retry_policy', 'runtime_assets', NOTE, output='note'),
            dependency_edge(definition, 'retry_policy', 'inventory', 'plan_inputs', COPY,
                            output='list', binding='inventory'),
            dependency_edge(definition, 'inventory', 'inventory_source', 'inputs', SOURCE,
                            output='list', binding='list'),
            dependency_edge(definition, 'inventory_source', 'scanner', 'inputs', REFERENCE,
                            output='executable', binding='scanner'),
            dependency_edge(definition, 'inventory_source', 'verifier', 'inputs', VERIFY,
                            output='executable', binding='verifier'),
        ])
        visible = html.unescape(shown.stdout)
        assert all(text in visible for text in (host, canonical, path, COPY, SOURCE, REFERENCE, VERIFY)), visible
        check_dependency_html(shown.stdout, plan)
        check_target_dependencies_html(shown.stdout, plan)
    assert scope_state(scope) == files and journal_contents(journal) == logged, 'show ran a project action'
    return receipt[1] if receipt else None


def check_watch(ycxx, scope, env, journal, host, other):
    watch = Watch(ycxx, scope, env, *target_args(other))
    try:
        summary, events, watched = watch.build('unchanged host-dependent planning input under watch')
        assert summary['actions_executed'] == 0, summary
        assert {str(scope / path) for path in (SCANNER, VERIFIER, SCORE_HEADER)} <= set(watched), watched
        check_external_actions(events)

        replace_source(scope, SCORE_HEADER, header(23))
        _, events, _ = watch.build('changed inventory selects a different native header')
        check_target_trace(events, other)
        check_external_actions(events, 'inventory_source', 'inventory', 'retry_policy')
        assert action_keys(events, 'RuleWorker').count('retry_policy') == 1, events
        assert 'executed' in tu_outcomes(events, 'main.cc'), events
        check_binary(scope, other, other, 3, host)
        inspect(ycxx, scope, env, journal, other, host, score=3)
        installed, default = published(scope, other), published(scope, None)
        assert journal_contents(journal) == '17\n18\n19\n23\n'

        replace_source(scope, SCORE_HEADER, f'#include "{REPAIR}"\n')
        summary, events, watched = watch.build('host inventory build discovers a missing include', succeeds=False)
        assert any('host executable' in item and 'file not found' in item
                   for item in summary['diagnostics']), summary
        assert str(scope / REPAIR) in watched, watched
        check_external_actions(events)
        assert published(scope, other) == installed and published(scope, None) == default
        inspect(ycxx, scope, env, journal, other, host)

        replace_source(scope, REPAIR, header(23))
        _, events, _ = watch.build('repair the missing include with unchanged inventory')
        check_external_actions(events)
        assert not action_keys(events, 'RuleWorker'), events
        assert published(scope, other) == installed and published(scope, None) == default
        assert journal_contents(journal) == '17\n18\n19\n23\n'
        check_binary(scope, other, other, 3, host)
        inspect(ycxx, scope, env, journal, other, host, score=3)
        watch.idle('watch after repairing the inventory scanner')
    finally:
        watch.close()


def check_builds(ycxx, scope, env, journal, host, other):
    prepare(scope, journal)
    inspect(ycxx, scope, env, journal, other, host, cold=True)
    _, events = build(ycxx, scope, env, other, other, 'read native-produced inventory for a foreign app')
    assert all('executed' in tu_outcomes(events, path) for path in (SCANNER, VERIFIER)), events
    check_external_actions(events, 'inventory_source', 'inventory', 'retry_policy')
    assert action_keys(events, 'RuleWorker').count('retry_policy') == 1, events
    check_binary(scope, other, other, 7, host)
    initial = inspect(ycxx, scope, env, journal, other, host, score=7)

    _, events = build(ycxx, scope, env, None, host, 'reuse the host inventory for the default target')
    assert not RAN & tu_outcomes(events, SCANNER), events
    check_external_actions(events)
    check_binary(scope, None, host, 7, host)
    assert journal_contents(journal) == '17\n'

    installed = published(scope, other)
    replace_source(scope, SCORE_HEADER, header(17) + '// Same native image.\n')
    inspect(ycxx, scope, env, journal, other, host)
    _, events = build(ycxx, scope, env, other, other, 'refresh stale host evidence with equal images')
    check_external_actions(events)
    assert not action_keys(events, 'RuleWorker'), events
    assert inspect(ycxx, scope, env, journal, other, host, score=7) == initial
    assert published(scope, other) == installed and journal_contents(journal) == '17\n'

    replace_source(scope, SCORE_HEADER, header(18))
    inspect(ycxx, scope, env, journal, other, host)
    _, events = build(ycxx, scope, env, other, other, 'change native image but retain selected inventory bytes')
    assert 'executed' in tu_outcomes(events, SCANNER), events
    check_external_actions(events, 'inventory_source')
    assert not action_keys(events, 'RuleWorker'), events
    assert inspect(ycxx, scope, env, journal, other, host, score=7) == initial
    assert published(scope, other) == installed and journal_contents(journal) == '17\n18\n'

    definition = (scope / 'build.ycxx').read_text(encoding='utf-8')
    copy_args = 'args = [input("list"), output("list")]'
    assert definition.count(copy_args) == 1, definition
    replace_source(scope, 'build.ycxx', definition.replace(copy_args, 'args = ["-p", input("list"), output("list")]'))
    _, events = build(ycxx, scope, env, other, other, 'refresh the planning receipt with equal inventory bytes')
    check_external_actions(events, 'inventory')
    assert 'retry_policy' not in action_keys(events, 'RuleWorker'), events
    assert not RAN & tu_outcomes(events, 'main.cc'), events
    copied = inspect(ycxx, scope, env, journal, other, host, score=7)
    assert copied != initial, 'show kept the previous copy action receipt'
    assert published(scope, other) == installed and journal_contents(journal) == '17\n18\n'

    replace_source(scope, SCORE_HEADER, header(19))
    _, events = build(ycxx, scope, env, other, other, 'change inventory whitespace but retain generated C++')
    check_external_actions(events, 'inventory_source', 'inventory')
    assert action_keys(events, 'RuleWorker').count('retry_policy') == 1, events
    assert not RAN & tu_outcomes(events, 'main.cc'), events
    assert inspect(ycxx, scope, env, journal, other, host, score=7) != copied
    assert published(scope, other) == installed and journal_contents(journal) == '17\n18\n19\n'
    check_watch(ycxx, scope, env, journal, host, other)


def check_planning_witness_race(ycxx, scope, env, journal, host, other, *, producer_failure=False):
    witness_runs = prepare_probe(scope)
    producers = prepare_producers(scope, journal)
    manifest = scope / 'build.ycxx'
    definition = manifest.read_text(encoding='utf-8')
    reads = 'reads = ["wire.cfg", "rules/check-mode.sh"]'
    assert definition.count(reads) == 1, definition
    definition = definition.replace(reads, f'plan_inputs = {{ inventory = {COPY} }}')
    script_target = 'witness_3' if producer_failure else 'witness_script'
    scripts = f'''\
generate "{script_target}" {{
    tool = "/bin/cp"
    args = ["-p", input("script"), output("tool")]
    inputs = {{ script = "witness.sh" }}
    outputs = {{ tool = "witness.sh" }}
}}
'''
    if producer_failure:
        scripts += '''\
generate "witness_7" {
    tool = "/bin/sh"
    args = [input("script")]
    inputs = { script = "fail-witness-producer.sh" }
    outputs = { tool = "witness.sh" }
}
'''
    manifest.write_text(definition + '\n' + producers + scripts, encoding='utf-8')

    hook = scope / 'witness.sh'
    sentinel = scope.with_name(f'{scope.name}-witness-edited')
    pending = scope.with_name(f'{scope.name}-witness-new-header')
    edit = f'''\
    printf '%s' {shlex.quote(header(23))} > {shlex.quote(str(pending))} || exit 1
    /bin/mv -f {shlex.quote(str(pending))} {shlex.quote(str(scope / SCORE_HEADER))} || exit 1
    : > {shlex.quote(str(sentinel))} || exit 1
'''
    hook.write_text(f'''\
#!/bin/sh
score=$(/bin/cat "$1") || exit 1
[ "$score" = "$2" ] || exit 42
printf '%s\\n' "$score" >> {shlex.quote(str(witness_runs))} || exit 1
if [ "$score" = 7 ] && [ ! -e {shlex.quote(str(sentinel))} ]; then
{edit}fi
printf '%s\\n' "$score"
''', encoding='utf-8')
    hook.chmod(0o700)
    failed_runs = scope.with_name(f'{scope.name}-failed-producer-runs')
    diagnostic = 'stale inventory selected a failing witness producer'
    if producer_failure:
        (scope / 'fail-witness-producer.sh').write_text(f'''\
#!/bin/sh
printf '%s\\n' failed >> {shlex.quote(str(failed_runs))} || exit 1
if [ ! -e {shlex.quote(str(sentinel))} ]; then
{edit}fi
printf '%s\\n' {shlex.quote(diagnostic)} >&2
exit 37
''', encoding='utf-8')

    rule = scope / 'rules/codec.cc'
    source = rule.read_text(encoding='utf-8')
    start, end = source.index('    tool = '), source.index('})ycxx";')
    selected_script = 'witness_@' if producer_failure else script_target
    source = source[:start] + f'''\
    tool = input("script")
    args = [input("data"), "@"]
    inputs = {{ script = artifact("{selected_script}", "tool") data = {COPY} }}
''' + source[end:]
    start, end = source.index('template<size_t Size>'), source.index('bool valid_host(')
    source = source[:start] + r'''bool inventory_score(const ycxx_rule_host_v1* host, char& score) {
    ycxx_rule_bytes_v1 bytes{};
    if (!host->read || host->read(host->context, "inventory", &bytes) != 1 ||
        bytes.size != 2 || bytes.data[1] != '\n') return false;
    score = bytes.data[0];
    return score == '7' || score == '3';
}

''' + source[end:]
    source = source.replace('    return host->emit(host->context, plan, sizeof(plan) - 1);', '''\
    char score{};
    if (!inventory_score(host, score)) return 2;
    char emitted[sizeof(plan)] = {};
    for (size_t i = 0; i < sizeof(plan) - 1; ++i) emitted[i] = plan[i] == '@' ? score : plan[i];
    return host->emit(host->context, emitted, sizeof(plan) - 1);''')
    start, end = source.index('    uint32_t support ='), source.index('    return host->export_support')
    source = source[:start] + r'''    char score{};
    if (!inventory_score(host, score) || witness.exit_status != 0 || witness.out.size != 2 ||
        witness.out.data[0] != score || witness.out.data[1] != '\n') return 2;
    const uint32_t support = score == '7' ? YCXX_RULE_SUPPORTED_V1 : YCXX_RULE_UNSUPPORTED_V1;
''' + source[end:]
    rule.write_text(source, encoding='utf-8')

    summary, events = build(ycxx, scope, env, other, other, 'replan after an in-flight shared host header edit')
    assert sentinel.exists() and (scope / SCORE_HEADER).read_text(encoding='utf-8') == header(23)
    assert journal_contents(journal) == '17\n23\n', journal_contents(journal)
    observations = '3\n' if producer_failure else '7\n3\n'
    assert journal_contents(witness_runs) == observations, journal_contents(witness_runs)
    assert action_keys(events, 'RuleWorker').count('wire') == 2, events
    assert action_keys(events, 'RuleWitness') == ['wire.fast-mode'] * observations.count('\n'), events
    assert action_keys(events, 'RuleClassifier') == ['wire.fast-mode'], events
    failed = ('witness_7',) if producer_failure else ()
    check_external_actions(events, 'inventory_source', 'inventory_source', 'inventory', 'inventory',
                           script_target, 'config', failed=failed)
    if producer_failure:
        assert journal_contents(failed_runs) == 'failed\n', journal_contents(failed_runs)
        assert all(diagnostic not in item for item in summary['diagnostics']), summary

    before = scope_state(scope)
    shown = run_client(ycxx, scope, env, 'show', 'app', *target_args(other), '--format=json')
    assert shown.returncode == 0, shown.stderr
    plan = json.loads(shown.stdout)
    assert plan['dependency_graph']['complete'], plan
    fact(plan, 'value-export', 'wire', 'unsupported')
    assert fact(plan, 'argv', 'wire', '3')['name'] == '1'
    assert scope_state(scope) == before and journal_contents(witness_runs) == observations

    _, events = build(ycxx, scope, env, None, host, 'reuse the current planning and witness receipts on the host')
    check_external_actions(events)
    assert not action_keys(events, 'RuleWitness'), events
    check_program(scope / executable(None), 10)


def main():
    ycxx = Path(sys.argv[1]).resolve()
    host, other = {'linux': (LINUX, DARWIN), 'darwin': (DARWIN, LINUX)}[sys.platform]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-host-planning-input-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        env = env_for(runtime)
        scope = root / 'project'
        try:
            check_builds(ycxx, scope, env, root / 'scanner-runs', host, other)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')
        for name, producer_failure in (('race', False), ('failed-producer', True)):
            scope = root / name
            try:
                check_planning_witness_race(ycxx, scope, env, root / f'{name}-scanner-runs', host, other,
                                            producer_failure=producer_failure)
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
