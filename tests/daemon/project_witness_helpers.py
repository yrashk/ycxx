"""Track absolute helper tools declared by a rule witness (#1526)."""

import html
import json
from pathlib import Path
import re
import stat
import sys
import tempfile

from project_build import action_keys, check_program, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_inspection import check_location, fact, scope_state
from project_shader_bundle import check_external_actions, check_native_reused, publication
from project_staged_tool import journal_bytes
from project_staged_witness import (
    ANALYZER, EXPORT, NATIVE, RULE, SELECTOR, check_observation, inspect as inspect_staged,
    prepare, score_value, value_key,
)
from runtime_dir import runtime_root


HELPER_NAME = 'score_helper'


def helper_script(score):
    return f"#!/bin/sh\nprintf '%s\\n' '{score}'\n"


def prepare_helper(scope, journal, *, absolute=False):
    prepare(scope, journal)
    helper = scope.with_name(scope.name + '-score-helper.sh')
    underlying = helper.with_name(scope.name + '-helper-implementation.sh')
    underlying.write_text(helper_script('7'), encoding='utf-8')
    underlying.chmod(0o700)
    helper.symlink_to(underlying.name)

    analyzer = scope / ANALYZER
    script = analyzer.read_text(encoding='utf-8')
    invocation = r"printf '%s\n' '7'"
    assert script.count(invocation) == 1, script
    analyzer.write_text(script.replace(invocation, 'exec "$1"'), encoding='utf-8')

    rule = scope / RULE
    source = rule.read_text(encoding='utf-8')
    selector = f'tool = {SELECTOR}'
    assert source.count(selector) == 1, source
    helper_arg = json.dumps(str(helper))
    command = f'tool = "/bin/sh"\n    args = [{SELECTOR}, {helper_arg}]' if absolute else (
        f'{selector}\n    args = [{helper_arg}]')
    source = source.replace(selector, command)
    rule.write_text(source, encoding='utf-8')
    return helper, source


def with_helpers(source, bindings):
    anchor = 'export = "score"'
    assert source.count(anchor) == 1, source
    return source.replace(anchor, f'tool_inputs = {{ {bindings} }}\n    {anchor}')


def named_helper(source, helper):
    return with_helpers(source, f'{HELPER_NAME} = {json.dumps(str(helper))}')


def inspect_helper(ycxx, scope, env, journal, helper, score, *, absolute=False):
    root = scope.parent
    before = scope_state(root)
    recorded = journal_bytes(journal)
    if absolute:
        result = run_client(ycxx, scope, env, 'show', 'app', '--format=json')
        assert result.returncode == 0, result.stderr
        plan = json.loads(result.stdout)
        assert plan['schema'] == 1 and plan['target'] == 'app' and plan['dependency_graph']['complete'], plan
        fact(plan, 'tool', 'observed', '/bin/sh')
        fact(plan, 'fact-input', 'config', score_value(score))
    else:
        plan = inspect_staged(ycxx, scope, env, journal, score)
    item = fact(plan, 'tool-input', 'observed', str(helper))
    assert item['name'] == HELPER_NAME, item
    check_location(item, plan['definition'], 'tools.observe "observed"')
    canonical = f'declared in tool_inputs · canonical {helper.resolve()} · fingerprint '
    fingerprint = re.fullmatch(re.escape(canonical) + r'([0-9a-f]{32})', item['origin'])
    assert fingerprint, item

    result = run_client(ycxx, scope, env, 'show', 'app', '--format=html')
    assert result.returncode == 0, result.stderr
    document = html.unescape(result.stdout)
    assert all(spelling in document for spelling in (str(helper), HELPER_NAME, item['origin'])), document
    assert journal_bytes(journal) == recorded and scope_state(root) == before, 'inspection changed state'
    return fingerprint[1]


def replace_helper(scope, helper, source, mode):
    pending = scope / '.ycxx' / 'watch-new-helper'
    pending.write_text(source, encoding='utf-8')
    pending.chmod(mode)
    pending.replace(helper)


def check_watch(ycxx, scope, env, journal, helper, original_fingerprint):
    selected = helper.resolve()
    mode = stat.S_IMODE(selected.stat().st_mode)
    executable = scope / NATIVE
    installed = publication(executable)
    recorded = journal_bytes(journal)
    watch = Watch(ycxx, scope, env)
    try:
        summary, events, watched = watch.build('watch declared and canonical witness helpers')
        assert {str(helper), str(selected)} <= set(watched), watched
        assert summary['actions_executed'] == 0, summary
        check_external_actions(events)
        check_native_reused(events, executable, installed)

        replace_helper(scope, selected, helper_script(' 7 '), mode)
        _, events, _ = watch.build('helper edit normalizes to the same typed score')
        check_external_actions(events)
        check_observation(events)
        stable_key = value_key(events, 7, 'cutoff')
        assert not action_keys(events, 'RuleWorker') and not action_keys(events, 'RuleValueArguments'), events
        check_native_reused(events, executable, installed)
        assert journal_bytes(journal) == recorded + b'ran\n'
        fingerprint = inspect_helper(ycxx, scope, env, journal, helper, 7)
        assert fingerprint != original_fingerprint, 'show kept the previous helper fingerprint'

        changed = helper_script('8')
        replace_helper(scope, selected, changed, mode)
        _, events, _ = watch.build('helper changes the typed score')
        check_external_actions(events, 'config')
        check_observation(events)
        assert value_key(events, 8, 'executed') == stable_key
        assert action_keys(events, 'RuleWorker') == ['config'], events
        assert 'executed' in tu_outcomes(events, 'main.cc') and action_keys(events, 'Link'), events
        check_program(executable, 8)
        published = publication(executable)
        assert published[0] != installed[0], 'helper edit kept the old app bytes'
        recorded += b'ran\nran\n'
        assert journal_bytes(journal) == recorded
        latest_fingerprint = inspect_helper(ycxx, scope, env, journal, helper, 8)
        assert latest_fingerprint != fingerprint

        selected.chmod(mode & ~0o111)
        summary, events, watched = watch.build('reject the helper with the same non-executable bytes', succeeds=False)
        assert str(helper) in watched, watched
        assert any(str(helper) in item and 'not executable' in item for item in summary['diagnostics']), summary
        check_external_actions(events)
        assert not action_keys(events, 'RuleWitness') and not action_keys(events, 'RuleClassifier'), events
        check_native_reused(events, executable, published)
        assert selected.read_text(encoding='utf-8') == changed
        assert journal_bytes(journal) == recorded, 'non-executable helper launched the analyzer'
        check_program(executable, 8)
        inspect_staged(ycxx, scope, env, journal)

        selected.chmod(mode)
        summary, events, _ = watch.build('restore the helper executable mode')
        assert summary['actions_executed'] == 0, summary
        check_external_actions(events)
        assert not action_keys(events, 'RuleWitness') and not action_keys(events, 'RuleClassifier'), events
        check_native_reused(events, executable, published)
        assert journal_bytes(journal) == recorded
        check_program(executable, 8)
        assert inspect_helper(ycxx, scope, env, journal, helper, 8) == latest_fingerprint
        watch.idle('recovery using the admitted helper receipt')
    finally:
        watch.close()


def check_receipts(ycxx, scope, env, journal, helper, source):
    _, events = traced_build(ycxx, scope, env, 'run the analyzer with an omitted helper record')
    check_observation(events)
    executable = scope / NATIVE
    installed = publication(executable)
    check_program(executable, 7)
    assert journal_bytes(journal) == b'ran\n'

    replace_source(scope, RULE, with_helpers(source, ''))
    _, events = traced_build(ycxx, scope, env, 'add an empty helper record to the same witness')
    assert not action_keys(events, 'RuleWitness'), 'empty helper record changed witness receipt identity'
    assert journal_bytes(journal) == b'ran\n', 'empty helper record relaunched the witness'
    check_external_actions(events)
    check_native_reused(events, executable, installed)

    replace_source(scope, RULE, named_helper(source, helper))
    _, events = traced_build(ycxx, scope, env, 'declare the executable helper')
    assert action_keys(events, 'RuleWitness') == [EXPORT], 'declared helper did not enter the witness receipt'
    assert journal_bytes(journal) == b'ran\nran\n'
    check_program(executable, 7)


def check_builds(ycxx, scope, env, journal):
    helper, source = prepare_helper(scope, journal)
    inspect_staged(ycxx, scope, env, journal, cold=True)
    check_receipts(ycxx, scope, env, journal, helper, source)
    fingerprint = inspect_helper(ycxx, scope, env, journal, helper, 7)
    check_watch(ycxx, scope, env, journal, helper, fingerprint)


def check_absolute(ycxx, scope, env, journal):
    helper, source = prepare_helper(scope, journal, absolute=True)
    check_receipts(ycxx, scope, env, journal, helper, source)
    inspect_helper(ycxx, scope, env, journal, helper, 7, absolute=True)


def main():
    ycxx = Path(sys.argv[1]).resolve()
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-witness-helpers-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        env = env_for(runtime)
        for name, check in (('staged', check_builds), ('absolute', check_absolute)):
            scope = root / name
            try:
                check(ycxx, scope, env, root / f'{name}-analyzer-runs')
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
