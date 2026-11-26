"""Verify host and selected triples in project actions and receipts (#1347, #1534, #1540)."""

import html
import json
from pathlib import Path
import re
import sys
import tempfile

from project_build import action_keys, check_program, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import replace_source
from project_inspection import check_location, fact, manifest_location, scope_state
from project_package_probe import prepare as prepare_probe
from project_shader_bundle import check_external_actions
from project_targets import (
    DARWIN, LINUX, LINUX_ALIAS, RAN, build, check_native_reused, executable, journal_contents, namespace, target_args,
)
from runtime_dir import runtime_root


ARGUMENT = 'target_triple()'
HOST_ARGUMENT = 'host_triple()'
BUILD_ENV = f'BUILD_TRIPLE = {HOST_ARGUMENT}'
TARGET_ENV = f'TARGET_TRIPLE = {ARGUMENT}'
GENERATED = 'selected.cc'
REFERENCE = 'artifact("selected", "source")'
NOTE = 'assets/app/target.txt'
SCORES = {LINUX: 17, DARWIN: 35}
COMMON = rf'''
[ "$#" -eq 3 ] || exit 1
[ "$1" = "$BUILD_TRIPLE" ] || exit 1
case "$BUILD_TRIPLE" in
    {LINUX}|{DARWIN}) ;;
    *) printf 'unexpected host %s\n' "$1" >&2; exit 1 ;;
esac
printf 'common %s\n' "$BUILD_TRIPLE" >> "$2" || exit 1
printf '#define COMMON_SCORE 4\n' > "$3"
'''
SELECTED = rf'''
[ "$#" -eq 5 ] || exit 1
[ "$1" = "$TARGET_TRIPLE" ] || exit 1
case "$TARGET_TRIPLE" in
    {LINUX}) macro=__x86_64__; score=13 ;;
    {DARWIN}) macro=__aarch64__; score=31 ;;
    *) printf 'unexpected target %s\n' "$1" >&2; exit 1 ;;
esac
printf '%s %s\n' "$BUILD_TRIPLE" "$TARGET_TRIPLE" >> "$2" || exit 1
/bin/cat "$3" > "$4" || exit 1
printf '#ifndef %s\n#error wrong architecture for selected source\n#endif\n' "$macro" >> "$4" || exit 1
printf 'int selected_score() {{ return COMMON_SCORE + %s; }}\n' "$score" >> "$4" || exit 1
printf '%s\n' "$TARGET_TRIPLE" > "$5"
'''


def prepare(scope, journal):
    scope.mkdir()
    (scope / 'main.cc').write_text('int selected_score(); int main() { return selected_score(); }\n', encoding='utf-8')
    definition = f'''\
generate "common" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(COMMON)}, "common", {HOST_ARGUMENT}, {json.dumps(str(journal))}, output("header")]
    environment = {{ {BUILD_ENV} }}
    outputs = {{ header = "common.h" }}
}}
generate "selected" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(SELECTED)}, "selected", {ARGUMENT}, {json.dumps(str(journal))},
            input("common"), output("source"), output("note")]
    environment = {{ {BUILD_ENV} {TARGET_ENV} }}
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


def check_environment_fact(plan, action, name, value, origin, location):
    row, = (row for row in plan['facts'] if (row['kind'], row['target'], row['name']) ==
            ('environment', action, name))
    assert (row['value'], row['origin']) == (value, origin), row
    assert (row['line'], row['column']) == (location['line'], location['column']), (row, location)


def literal_environment(definition, host, target):
    return definition.replace(BUILD_ENV, f'BUILD_TRIPLE = {json.dumps(host)}').replace(
        TARGET_ENV, f'TARGET_TRIPLE = {json.dumps(target)}')


def inspect(ycxx, scope, env, journal, selector, canonical, host, *, cold=False, render=False,
            target_literal=False, host_literal=False, env_literal=False):
    before = journal_contents(journal)
    files = scope_state(scope)
    result = run_client(ycxx, scope, env, 'show', 'app', *target_args(selector), '--format=json')
    assert result.returncode == 0, (selector, result.stderr)
    plan = json.loads(result.stdout)
    config = [row for row in plan['facts'] if row['kind'] == 'native-target']
    assert [row['value'] for row in config] == [canonical], plan
    if cold:
        assert not (scope / '.ycxx').exists(), 'cold show created build state'
        assert journal_contents(journal) == before, 'cold show ran an action'
        assert scope_state(scope) == files, 'cold show wrote to the workspace'
        return None

    for action, value, expression, literal in (
        ('selected', canonical, ARGUMENT, target_literal), ('common', host, HOST_ARGUMENT, host_literal),
    ):
        argument = fact(plan, 'argv', action, value)
        origin = 'declared in args' if literal else expression
        assert (argument['name'], argument['origin']) == ('3', origin), argument
        check_location(argument, plan['definition'], json.dumps(value) if literal else expression)
    definition = plan['definition']
    for action, name, value, expression in (
        ('common', 'BUILD_TRIPLE', host, HOST_ARGUMENT),
        ('selected', 'BUILD_TRIPLE', host, HOST_ARGUMENT),
        ('selected', 'TARGET_TRIPLE', canonical, ARGUMENT),
    ):
        prefix = f'{name} = '
        spelling = json.dumps(value) if env_literal else expression
        start = definition.index(prefix + spelling, definition.index(f'generate "{action}"'))
        origin = 'declared in environment' if env_literal else expression
        check_environment_fact(plan, action, name, value, origin, manifest_location(definition, start + len(prefix)))
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
            assert all(term in visible for term in (
                canonical, host, ARGUMENT, HOST_ARGUMENT, 'BUILD_TRIPLE', 'TARGET_TRIPLE', source_path,
            )), visible
    assert journal_contents(journal) == before, 'show ran an action'
    assert scope_state(scope) == files, 'show wrote to the workspace'
    return fingerprint[1]


def check_action_trace(events, action, expression, canonical):
    generated = [event for event in events if event.get('kind') == 'ExternalAction'
                 and event['key'] == action and event['outcome'] == 'executed']
    assert len(generated) == 1 and f'args[3]: {expression} = {canonical}' in generated[0].get('value', ''), generated


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
        inspect(ycxx, scope, env, journal, selector, selector or host, host, cold=True)

    _, events = build(ycxx, scope, env, None, host, 'cold host target argument')
    check_external_actions(events, 'common', 'selected')
    check_action_trace(events, 'common', HOST_ARGUMENT, host)
    check_action_trace(events, 'selected', ARGUMENT, host)
    assert 'executed' in tu_outcomes(events, GENERATED) and action_keys(events, 'Link'), events
    check_products(scope, None, host, host)
    host_receipt = inspect(ycxx, scope, env, journal, None, host, host, render=True)

    summary, events = build(ycxx, scope, env, host, host, 'explicit host reuses host-default action receipts')
    assert summary['actions_executed'] == 0, summary
    check_external_actions(events)
    check_products(scope, host, host, host)
    assert inspect(ycxx, scope, env, journal, host, host, host) == host_receipt

    _, events = build(ycxx, scope, env, other, other, 'different target executes only the target-sensitive generator')
    check_external_actions(events, 'selected')
    assert not action_keys(events, 'RulePackage'), events
    check_action_trace(events, 'selected', ARGUMENT, other)
    assert 'executed' in tu_outcomes(events, GENERATED) and action_keys(events, 'Link'), events
    check_products(scope, other, other, host)
    cross_receipt = inspect(ycxx, scope, env, journal, other, other, host, render=True)
    assert cross_receipt != host_receipt, (host_receipt, cross_receipt)

    receipts = {host: host_receipt, other: cross_receipt}
    for selector, canonical in ((LINUX_ALIAS, LINUX), (DARWIN, DARWIN), (None, host), (LINUX, LINUX)):
        summary, events = build(ycxx, scope, env, selector, canonical, 'alternate cached target arguments')
        assert summary['actions_executed'] == 0, summary
        check_external_actions(events)
        check_native_reused(events)
        assert not (RAN & tu_outcomes(events, GENERATED)), events
        assert inspect(ycxx, scope, env, journal, selector, canonical, host) == receipts[canonical]
        check_products(scope, canonical if selector else None, canonical, host)

    authored = (scope / 'build.ycxx').read_text(encoding='utf-8')
    assert authored.count(f', {ARGUMENT},') == authored.count(f', {HOST_ARGUMENT},') == 1, authored
    replace_source(scope, 'build.ycxx', authored.replace(f', {HOST_ARGUMENT},', f', {json.dumps(host)},'))
    for selector in (None, other):
        canonical = selector or host
        _, events = build(ycxx, scope, env, selector, canonical, 'literal argv equals the resolved host argument')
        # Editing a declaration can relaunch its rule worker while the resolved action receipt stays green.
        check_external_actions(events)
        check_native_reused(events)
        assert inspect(ycxx, scope, env, journal, selector, canonical, host, host_literal=True) == receipts[canonical]

    replace_source(scope, 'build.ycxx', authored.replace(f', {ARGUMENT},', f', {json.dumps(host)},'))
    _, events = build(ycxx, scope, env, None, host, 'literal argv equals the resolved target argument')
    check_external_actions(events)
    check_native_reused(events)
    assert not RAN & tu_outcomes(events, GENERATED), events
    assert inspect(ycxx, scope, env, journal, None, host, host, target_literal=True) == host_receipt

    for selector in (None, other):
        canonical = selector or host
        replace_source(scope, 'build.ycxx', literal_environment(authored, host, canonical))
        _, events = build(ycxx, scope, env, selector, canonical, 'literal environment equals the resolved triples')
        check_external_actions(events)
        check_native_reused(events)
        assert not RAN & tu_outcomes(events, GENERATED), events
        assert inspect(ycxx, scope, env, journal, selector, canonical, host, env_literal=True) == receipts[canonical]
        check_products(scope, selector, canonical, host)
    assert journal_contents(journal) == f'common {host}\n{host} {host}\n{host} {other}\n', 'extra generator execution'


def check_witness(ycxx, scope, env, host, other):
    journal = prepare_probe(scope)
    rule = scope / 'rules/codec.cc'
    source = rule.read_text(encoding='utf-8')
    args = 'args = [input("check"), input("data")]'
    assert source.count(args) == 1, source
    definition = source.replace(args, args[:-1] + f', {ARGUMENT}, {HOST_ARGUMENT}]\n'
                                f'    environment = {{ {BUILD_ENV} {TARGET_ENV} }}')
    rule.write_text(definition, encoding='utf-8')
    probe = scope / 'rules/check-mode.sh'
    source = probe.read_text(encoding='utf-8')
    anchor = 'case "$mode" in'
    assert source.count(anchor) == 1, source
    choose = f'''\
[ "$3" = "{host}" ] || exit 2
[ "$BUILD_TRIPLE" = "{host}" ] || exit 2
[ "$2" = "$TARGET_TRIPLE" ] || exit 2
case "$TARGET_TRIPLE" in
    {LINUX}) mode=enabled ;;
    {DARWIN}) mode=disabled ;;
    *) exit 2 ;;
esac
'''
    probe.write_text(source.replace(anchor, choose + anchor), encoding='utf-8')

    def observe(selector, canonical, *, host_literal=False, env_literal=False):
        logged = journal_contents(journal)
        files = scope_state(scope)
        result = run_client(ycxx, scope, env, 'show', 'app', *target_args(selector), '--format=json')
        assert result.returncode == 0, result.stderr
        plan = json.loads(result.stdout)
        state = 'supported' if canonical == LINUX else 'unsupported'
        fact(plan, 'value-export', 'wire', state)
        arguments = {row['name']: row for row in plan['facts'] if (row['kind'], row['target']) == ('argv', 'wire')}
        expected = (('2', canonical, ARGUMENT), ('3', host, 'declared in args' if host_literal else HOST_ARGUMENT))
        for index, value, origin in expected:
            row = arguments[index]
            assert (row['value'], row['origin']) == (value, origin), row
            check_location(row, plan['definition'], 'codec.profile "wire"')
        authored = plan['definition']
        site = manifest_location(authored, authored.index('codec.profile "wire"'))
        for name, value, expression in (('BUILD_TRIPLE', host, HOST_ARGUMENT), ('TARGET_TRIPLE', canonical, ARGUMENT)):
            origin = 'declared in environment' if env_literal else expression
            check_environment_fact(plan, 'wire', name, value, origin, site)
        assert journal_contents(journal) == logged and scope_state(scope) == files, 'show ran the witness'

    try:
        for selector, canonical, runs in ((None, host, True), (other, other, True),
                                          (host, host, False), (other, other, False)):
            summary, events = build(ycxx, scope, env, selector, canonical, 'configured package witness')
            witnesses = [event for event in events if event.get('kind') == 'RuleWitness'
                         and event['key'] == 'wire.fast-mode' and event['outcome'] == 'executed']
            if runs:
                assert len(witnesses) == 1, witnesses
                value = witnesses[0]['value']
                assert f'args[2]: {ARGUMENT} = {canonical}' in value, value
                assert f'args[3]: {HOST_ARGUMENT} = {host}' in value, value
            else:
                assert summary['actions_executed'] == 0 and not witnesses, (summary, witnesses)
            observe(selector, canonical)

        replace_source(scope, 'rules/codec.cc', definition.replace(f', {HOST_ARGUMENT}]', f', {json.dumps(host)}]'))
        for selector in (None, other):
            canonical = selector or host
            _, events = build(ycxx, scope, env, selector, canonical, 'literal argv equals the witness host argument')
            assert not action_keys(events, 'RuleWitness'), 'equivalent literal relaunched the witness'
            observe(selector, canonical, host_literal=True)

        for selector in (None, other):
            canonical = selector or host
            replace_source(scope, 'rules/codec.cc', literal_environment(definition, host, canonical))
            _, events = build(ycxx, scope, env, selector, canonical, 'literal environment equals the witness triples')
            assert not action_keys(events, 'RuleWitness'), 'equivalent environment relaunched the witness'
            observe(selector, canonical, env_literal=True)
        assert journal_contents(journal) == 'enabled\nenabled\n', 'extra witness execution'
    finally:
        run_client(ycxx, scope, env, 'ctl', 'shutdown')


def check_failure(ycxx, scope, env, target, label, expected, journal):
    try:
        summary, events = traced_build(ycxx, scope, env, label, *target_args(target), succeeds=False)
        assert any(expected in diagnostic for diagnostic in summary['diagnostics']), summary
        check_external_actions(events)
        assert not journal.exists(), 'invalid recipe ran a tool'
    finally:
        run_client(ycxx, scope, env, 'ctl', 'shutdown')


def check_rejections(ycxx, workspace, env, other):
    environment = f'environment = {{ {BUILD_ENV} }}'
    for name, value, expected in (
        ('env-input', 'input("script")', 'expected an environment value'),
        ('env-artifact', REFERENCE, 'expected an environment value'),
        ('env-arity', 'host_triple("extra")', "expected ')'"),
    ):
        scope = workspace / name
        journal = workspace / f'{name}-runs'
        definition = prepare(scope, journal)
        assert definition.count(environment) == 1, definition
        invalid = definition.replace(environment, f'environment = {{ BUILD_TRIPLE = {value} }}')
        (scope / 'build.ycxx').write_text(invalid, encoding='utf-8')
        check_failure(ycxx, scope, env, other, name, expected, journal)

    for expression in (ARGUMENT, HOST_ARGUMENT):
        name = expression.removesuffix('()')
        for case, original, replacement, expected in (
            ('arity', f', {expression},', f', {name}("extra"),', "expected ')'"),
            ('field', f'source = "{GENERATED}"', f'source = {expression}', 'an output filename'),
        ):
            scope = workspace / f'{name}-{case}'
            journal = workspace / f'{name}-{case}-runs'
            definition = prepare(scope, journal)
            assert definition.count(original) == 1, definition
            (scope / 'build.ycxx').write_text(definition.replace(original, replacement), encoding='utf-8')
            check_failure(ycxx, scope, env, other, case, expected, journal)

        for case, argument, setting, expected in (
            ('argument', expression, '', f'{expression} is only available in project action arguments'),
            ('environment', '"fallback"', f'    environment = {{ BUILD_TRIPLE = {expression} }}\n',
             'expected an environment value'),
        ):
            scope = workspace / f'{name}-host-tool-{case}'
            journal = workspace / f'{name}-host-tool-{case}-runs'
            (scope / 'rules').mkdir(parents=True)
            (scope / 'build.ycxx').write_text('use plugin("rules/package") as tools\ntools.app "app" {}\n',
                                              encoding='utf-8')
            script = r'''printf '%s\n' "$1" > "$2"; printf 'not a plugin' > "$3"'''
            (scope / 'rules/build.ycxx').write_text(f'''\
host_action "package" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(script)}, "forbidden", {argument}, {json.dumps(str(journal))}, output("plugin")]
{setting}    outputs = {{ plugin = "rules.so" }}
}}
''', encoding='utf-8')
            check_failure(ycxx, scope, env, other, f'host bootstrap triple {case}', expected, journal)


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
