"""Verify target_triple() arguments in project actions and planning receipts (#1347)."""

import html
import json
from pathlib import Path
import re
import sys
import tempfile

from project_build import action_keys, check_program, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import replace_source
from project_inspection import check_location, fact
from project_package_probe import prepare as prepare_probe
from project_shader_bundle import check_external_actions
from project_targets import (
    DARWIN, LINUX, LINUX_ALIAS, RAN, build, check_native_reused, executable, journal_contents, namespace, target_args,
)
from runtime_dir import runtime_root


ARGUMENT = 'target_triple()'
GENERATED = 'selected.cc'
REFERENCE = 'artifact("selected", "source")'
NOTE = 'assets/app/target.txt'
SCORES = {LINUX: 17, DARWIN: 35}
COMMON = r'''
printf 'common\n' >> "$1" || exit 1
printf '#define COMMON_SCORE 4\n' > "$2"
'''
SELECTED = rf'''
[ "$#" -eq 5 ] || exit 1
case "$1" in
    {LINUX}) macro=__x86_64__; score=13 ;;
    {DARWIN}) macro=__aarch64__; score=31 ;;
    *) printf 'unexpected target %s\n' "$1" >&2; exit 1 ;;
esac
printf '%s\n' "$1" >> "$2" || exit 1
/bin/cat "$3" > "$4" || exit 1
printf '#ifndef %s\n#error wrong architecture for selected source\n#endif\n' "$macro" >> "$4" || exit 1
printf 'int selected_score() {{ return COMMON_SCORE + %s; }}\n' "$score" >> "$4" || exit 1
printf '%s\n' "$1" > "$5"
'''


def prepare(scope, journal):
    scope.mkdir()
    (scope / 'main.cc').write_text('int selected_score(); int main() { return selected_score(); }\n', encoding='utf-8')
    definition = f'''\
generate "common" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(COMMON)}, "common", {json.dumps(str(journal))}, output("header")]
    outputs = {{ header = "common.h" }}
}}
generate "selected" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(SELECTED)}, "selected", {ARGUMENT}, {json.dumps(str(journal))},
            input("common"), output("source"), output("note")]
    inputs = {{ common = artifact("common", "header") }}
    outputs = {{ source = "{GENERATED}", note = "target.txt" }}
}}
cpp.executable "app" {{
    plan_inputs = {{ code = {REFERENCE} }}
    sources = ["main.cc", {REFERENCE}]
    runtime_assets = [artifact("selected", "note")]
}}
'''
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    return definition


def inspect(ycxx, scope, env, journal, selector, canonical, *, cold=False, render=False, literal=False):
    before = journal_contents(journal)
    result = run_client(ycxx, scope, env, 'show', 'app', *target_args(selector), '--format=json')
    assert result.returncode == 0, (selector, result.stderr)
    plan = json.loads(result.stdout)
    config = [row for row in plan['facts'] if row['kind'] == 'native-target']
    assert [row['value'] for row in config] == [canonical], plan
    if cold:
        assert not (scope / '.ycxx').exists(), 'cold show created build state'
        assert journal_contents(journal) == before, 'cold show ran an action'
        return None

    argument = fact(plan, 'argv', 'selected', canonical)
    origin = 'declared in args' if literal else ARGUMENT
    assert (argument['name'], argument['origin']) == ('3', origin), argument
    check_location(argument, plan['definition'], json.dumps(canonical) if literal else ARGUMENT)
    selected = canonical if selector else None
    source_path = str(namespace(selected) / 'virtual/selected' / GENERATED)
    fact(plan, 'source', 'app', source_path)
    receipt = fact(plan, 'plan-input', 'app', source_path)
    assert receipt['name'] == 'code', receipt
    fingerprint = re.fullmatch(re.escape(f'{REFERENCE} · receipt ') + r'([0-9a-f]{32})', receipt['origin'])
    assert fingerprint, receipt
    fact(plan, 'runtime-asset', 'app', str(namespace(selected) / NOTE))

    if render:
        for format_ in ('text', 'html'):
            shown = run_client(ycxx, scope, env, 'show', 'app', *target_args(selector), f'--format={format_}')
            assert shown.returncode == 0, shown.stderr
            visible = html.unescape(shown.stdout)
            assert all(term in visible for term in (canonical, ARGUMENT, source_path)), visible
    assert journal_contents(journal) == before, 'show ran an action'
    return fingerprint[1]


def check_action_trace(events, canonical):
    generated = [event for event in events if event.get('kind') == 'ExternalAction'
                 and event['key'] == 'selected' and event['outcome'] == 'executed']
    assert len(generated) == 1 and f'{ARGUMENT} = {canonical}' in generated[0].get('value', ''), generated


def check_products(scope, selector, canonical, host):
    path = scope / executable(selector)
    assert path.is_file(), path
    note = (scope / namespace(selector) / NOTE).read_text(encoding='utf-8')
    assert note == f'{canonical}\n', (selector, note)
    # PARITY: by design: execute only the target format supported by this host kernel.
    if canonical == host:
        check_program(path, SCORES[host])


def check_builds(ycxx, scope, env, journal, host, other):
    for selector in (None, other):
        inspect(ycxx, scope, env, journal, selector, selector or host, cold=True)

    _, events = build(ycxx, scope, env, None, host, 'cold host target argument')
    check_external_actions(events, 'common', 'selected')
    check_action_trace(events, host)
    assert 'executed' in tu_outcomes(events, GENERATED) and action_keys(events, 'Link'), events
    check_products(scope, None, host, host)
    host_receipt = inspect(ycxx, scope, env, journal, None, host, render=True)

    summary, events = build(ycxx, scope, env, host, host, 'explicit host reuses host-default action receipts')
    assert summary['actions_executed'] == 0, summary
    check_external_actions(events)
    check_products(scope, host, host, host)
    assert inspect(ycxx, scope, env, journal, host, host) == host_receipt

    _, events = build(ycxx, scope, env, other, other, 'different target executes only the target-sensitive generator')
    check_external_actions(events, 'selected')
    assert not action_keys(events, 'RulePackage'), events
    check_action_trace(events, other)
    assert 'executed' in tu_outcomes(events, GENERATED) and action_keys(events, 'Link'), events
    check_products(scope, other, other, host)
    cross_receipt = inspect(ycxx, scope, env, journal, other, other, render=True)
    assert cross_receipt != host_receipt, (host_receipt, cross_receipt)

    receipts = {host: host_receipt, other: cross_receipt}
    for selector, canonical in ((LINUX_ALIAS, LINUX), (DARWIN, DARWIN), (None, host), (LINUX, LINUX)):
        summary, events = build(ycxx, scope, env, selector, canonical, 'alternate cached target arguments')
        assert summary['actions_executed'] == 0, summary
        check_external_actions(events)
        check_native_reused(events)
        assert not (RAN & tu_outcomes(events, GENERATED)), events
        assert inspect(ycxx, scope, env, journal, selector, canonical) == receipts[canonical]
        check_products(scope, canonical if selector else None, canonical, host)

    authored = (scope / 'build.ycxx').read_text(encoding='utf-8')
    assert authored.count(ARGUMENT) == 1, authored
    replace_source(scope, 'build.ycxx', authored.replace(ARGUMENT, json.dumps(host)))
    _, events = build(ycxx, scope, env, None, host, 'literal argv equals the resolved target argument')
    check_external_actions(events)
    check_native_reused(events)
    assert not RAN & tu_outcomes(events, GENERATED), events
    assert inspect(ycxx, scope, env, journal, None, host, literal=True) == host_receipt
    assert journal_contents(journal) == f'common\n{host}\n{other}\n', 'extra generator execution'


def check_witness(ycxx, scope, env, host, other):
    journal = prepare_probe(scope)
    rule = scope / 'rules/codec.cc'
    source = rule.read_text(encoding='utf-8')
    args = 'args = [input("check"), input("data")]'
    assert source.count(args) == 1, source
    rule.write_text(source.replace(args, args[:-1] + f', {ARGUMENT}]'), encoding='utf-8')
    probe = scope / 'rules/check-mode.sh'
    source = probe.read_text(encoding='utf-8')
    anchor = 'case "$mode" in'
    assert source.count(anchor) == 1, source
    choose = f'''\
case "$2" in
    {LINUX}) mode=enabled ;;
    {DARWIN}) mode=disabled ;;
    *) exit 2 ;;
esac
'''
    probe.write_text(source.replace(anchor, choose + anchor), encoding='utf-8')
    try:
        for selector, canonical, runs in ((None, host, True), (other, other, True),
                                          (host, host, False), (other, other, False)):
            summary, events = build(ycxx, scope, env, selector, canonical, 'configured package witness')
            witnesses = [event for event in events if event.get('kind') == 'RuleWitness'
                         and event['key'] == 'wire.fast-mode' and event['outcome'] == 'executed']
            if runs:
                assert len(witnesses) == 1 and f'{ARGUMENT} = {canonical}' in witnesses[0]['value'], witnesses
            else:
                assert summary['actions_executed'] == 0 and not witnesses, (summary, witnesses)
            logged = journal_contents(journal)
            result = run_client(ycxx, scope, env, 'show', 'app', *target_args(selector), '--format=json')
            assert result.returncode == 0, result.stderr
            state = 'supported' if canonical == LINUX else 'unsupported'
            fact(json.loads(result.stdout), 'value-export', 'wire', state)
            assert journal_contents(journal) == logged, 'show ran the witness'
        assert journal_contents(journal) == 'enabled\nenabled\n', 'extra witness execution'
    finally:
        run_client(ycxx, scope, env, 'ctl', 'shutdown')


def check_failure(ycxx, scope, env, target, label, expected, journal):
    try:
        summary, events = traced_build(ycxx, scope, env, label, *target_args(target), succeeds=False)
        assert any(expected in diagnostic for diagnostic in summary['diagnostics']), summary
        check_external_actions(events)
        assert not journal.exists(), 'invalid argument ran the generator'
    finally:
        run_client(ycxx, scope, env, 'ctl', 'shutdown')


def check_rejections(ycxx, workspace, env, other):
    for name, original, replacement, expected in (
        ('arity', ARGUMENT, 'target_triple("extra")', "expected ')'"),
        ('field', f'source = "{GENERATED}"', 'source = target_triple()', 'an output filename'),
    ):
        scope = workspace / name
        journal = workspace / f'{name}-runs'
        definition = prepare(scope, journal)
        assert definition.count(original) == 1, definition
        (scope / 'build.ycxx').write_text(definition.replace(original, replacement), encoding='utf-8')
        check_failure(ycxx, scope, env, other, name, expected, journal)

    scope = workspace / 'host-tool'
    journal = workspace / 'host-tool-runs'
    (scope / 'rules').mkdir(parents=True)
    (scope / 'build.ycxx').write_text('use plugin("rules/package") as tools\ntools.app "app" {}\n', encoding='utf-8')
    script = r'''printf '%s\n' "$1" > "$2"; printf 'not a plugin' > "$3"'''
    (scope / 'rules/build.ycxx').write_text(f'''\
host_action "package" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(script)}, "forbidden", target_triple(), {json.dumps(str(journal))}, output("plugin")]
    outputs = {{ plugin = "rules.so" }}
}}
''', encoding='utf-8')
    check_failure(ycxx, scope, env, other, 'host bootstrap target argument',
                  'target_triple() is only available in project action arguments', journal)


def main():
    ycxx = Path(sys.argv[1]).resolve()
    host, other = {'linux': (LINUX, DARWIN), 'darwin': (DARWIN, LINUX)}[sys.platform]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-project-action-target-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        scope = root / 'project'
        journal = root / 'runs'
        env = env_for(runtime)
        prepare(scope, journal)
        try:
            check_builds(ycxx, scope, env, journal, host, other)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')
        check_witness(ycxx, root / 'witness', env, host, other)
        check_rejections(ycxx, root, env, other)


if __name__ == '__main__':
    main()
