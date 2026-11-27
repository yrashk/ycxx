"""A package owns a witnessed support value consumed by native C++ (#1257)."""

import json
from pathlib import Path
import shlex
import shutil
import sys
import tempfile

from project_build import action_keys, check_program, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_inspection import (
    check_dependency_graph, check_value_bindings_html, compare_value_bindings, dependency_edge, fact,
)
from runtime_dir import runtime_root


EXAMPLE = Path(__file__).resolve().parents[2] / 'examples/project-build/package-probe'
EXPORT = 'wire.fast-mode'
VALUE_FACTS = {('value-export', 'wire', 'fast-mode'), ('capability-input', 'config', 'fast')}


def prepare(scope):
    shutil.copytree(EXAMPLE, scope, ignore=shutil.ignore_patterns('.ycxx'))
    log = scope / 'witness-runs'
    script = scope / 'rules/check-mode.sh'
    source = script.read_text(encoding='utf-8')
    anchor = 'case "$mode" in'
    assert source.count(anchor) == 1, source
    record = f'''printf '%s\\n' "$evidence" >> {shlex.quote(str(log))}\n'''
    script.write_text(source.replace(anchor, record + anchor), encoding='utf-8')
    return log


def inspect(ycxx, scope, env, log, state=None, *, stale=False):
    recorded = log.read_bytes() if log.exists() else None
    result = run_client(ycxx, scope, env, 'show', 'app', '--format=json')
    assert result.returncode == 0, result.stderr
    assert (log.read_bytes() if log.exists() else None) == recorded, 'inspection ran the witness'
    plan = json.loads(result.stdout)
    facts = plan['facts']
    rows = [row for row in facts if (row['kind'], row['target'], row['name']) in VALUE_FACTS]
    if state is None:
        assert not rows, rows
        assert any(row['kind'] == 'unresolved' and (not stale or row['target'] == 'wire')
                   for row in facts), facts
        return

    assert len(rows) == len(VALUE_FACTS), rows
    assert {row['kind']: row['value'] for row in rows} == {
        'value-export': state, 'capability-input': state,
    }, rows
    origins = {row['kind']: row['origin'] for row in rows}
    assert all(term in origins['value-export'] for term in ('rules/package', '/bin/sh')), rows
    assert EXPORT in origins['capability-input'], rows
    definition = plan['definition']
    header = dependency_edge(definition, 'app', 'config', 'generated_headers',
                             'artifact("config", "header")', output='header')
    capability = {**dependency_edge(definition, 'config', 'wire', 'capabilities',
                                    'value("wire", "fast-mode")', binding='fast'),
                  'export_name': 'fast-mode'}
    check_dependency_graph(plan, [header], value_edges=[capability])
    return plan


def value_key(events, state, outcome):
    values = [event for event in events if event.get('kind') == 'RuleValue']
    matching = {event['key'] for event in values
                if event.get('value') == state and event['outcome'] == outcome}
    assert len(matching) == 1, (state, outcome, values)
    return matching.pop()


def check_observation(events):
    assert action_keys(events, 'RuleWitness') == [EXPORT], events
    assert action_keys(events, 'RuleClassifier') == [EXPORT], events


def check_generation(events):
    assert 'config' in action_keys(events, 'RuleWorker'), events
    assert action_keys(events, 'ExternalAction') == ['config'], events
    assert 'executed' in tu_outcomes(events, 'main.cc'), events
    assert action_keys(events, 'Link'), events


def check_reuse(events):
    assert 'config' not in action_keys(events, 'RuleWorker'), events
    for kind in ('RuleValueArguments', 'ExternalAction', 'Link'):
        assert not action_keys(events, kind), (kind, events)
    assert not {'executed', 'cutoff'} & tu_outcomes(events, 'main.cc'), events


def check_edits(ycxx, scope, env, log):
    executable = scope / '.ycxx/out/app'
    inspect(ycxx, scope, env, log)
    assert not (scope / '.ycxx').exists(), 'cold inspection created build state'

    _, events = traced_build(ycxx, scope, env, 'build the package probe and app')
    package_tools = [key for key in action_keys(events, 'RulePackage') if key.startswith('rules/package via ')]
    assert len(package_tools) == 1, package_tools
    if sys.platform == 'linux':
        assert package_tools == [f'rules/package via {ycxx}'], package_tools
    check_observation(events)
    check_generation(events)
    stable_key = value_key(events, 'supported', 'executed')
    assert log.read_bytes() == b'enabled\n'
    check_program(executable, 0)
    supported = inspect(ycxx, scope, env, log, 'supported')
    recorded = log.read_bytes()
    document = run_client(ycxx, scope, env, 'show', 'app', '--format=html')
    assert document.returncode == 0, document.stderr
    check_value_bindings_html(document.stdout, supported)
    assert log.read_bytes() == recorded, 'HTML inspection ran the witness'
    first = executable.read_bytes()
    installed_at = executable.stat().st_mtime_ns

    summary, events = traced_build(ycxx, scope, env, 'unchanged package probe')
    assert summary['actions_executed'] == 0, summary
    check_reuse(events)
    assert log.read_bytes() == b'enabled\n'
    assert executable.stat().st_mtime_ns == installed_at

    replace_source(scope, 'wire.cfg', '  enabled \n')
    inspect(ycxx, scope, env, log, stale=True)
    _, events = traced_build(ycxx, scope, env, 'changed witness evidence with equal support')
    check_observation(events)
    assert value_key(events, 'supported', 'cutoff') == stable_key
    check_reuse(events)
    assert log.read_bytes() == b'enabled\n  enabled \n'
    assert executable.read_bytes() == first and executable.stat().st_mtime_ns == installed_at
    check_program(executable, 0)
    inspect(ycxx, scope, env, log, 'supported')

    replace_source(scope, 'wire.cfg', 'disabled\n')
    _, events = traced_build(ycxx, scope, env, 'normal nonzero witness exit changes support')
    check_observation(events)
    assert value_key(events, 'unsupported', 'executed') == stable_key
    check_generation(events)
    unsupported = inspect(ycxx, scope, env, log, 'unsupported')
    assert unsupported['dependency_graph']['value_edges'] == supported['dependency_graph']['value_edges']
    check_program(executable, 10)

    watch = Watch(ycxx, scope, env)
    try:
        summary, _, watched = watch.build('watch current package probe')
        assert summary['actions_executed'] == 0, summary
        profile = scope / 'wire.cfg'
        assert str(profile) in watched and str(scope / 'rules/check-mode.sh') in watched, watched
        previous = executable.read_bytes()
        recorded = log.read_bytes()
        profile.unlink()
        summary, events, watched = watch.build('missing staged witness input', succeeds=False)
        assert any('wire.cfg' in diagnostic for diagnostic in summary['diagnostics']), summary
        assert str(profile) in watched, watched
        assert not action_keys(events, 'RuleClassifier') and log.read_bytes() == recorded, events
        assert executable.read_bytes() == previous
        check_program(executable, 10)
        inspect(ycxx, scope, env, log, stale=True)

        replace_source(scope, 'wire.cfg', 'unknown\n')
        _, events, _ = watch.build('restore staged witness input')
        check_observation(events)
        assert value_key(events, 'indeterminate', 'executed') == stable_key
        check_generation(events)
        check_program(executable, 20)
        inspect(ycxx, scope, env, log, 'indeterminate')
        watch.idle('inspection and publication after recovery')
        return executable.read_bytes()
    finally:
        watch.close()


def check_invalid_classifier(ycxx, scope, env, log, executable, previous):
    plugin = scope / 'rules/codec.cc'
    source = plugin.read_text(encoding='utf-8')
    call = 'return host->export_support(host->context, witness.name, support);'
    assert source.count(call) == 1, source
    duplicate = ('host->export_support(host->context, witness.name, support);\n    ' + call)
    replace_source(scope, 'rules/codec.cc', source.replace(call, duplicate))
    summary, events = traced_build(ycxx, scope, env, 'duplicate classifier export', succeeds=False)
    assert any('classif' in diagnostic.lower() for diagnostic in summary['diagnostics']), summary
    assert action_keys(events, 'RuleClassifier', 'failed') == [EXPORT], events
    assert not action_keys(events, 'ExternalAction') and not action_keys(events, 'Link'), events
    assert executable.read_bytes() == previous
    inspect(ycxx, scope, env, log, stale=True)


def check_manifest_edit(ycxx, scope, env):
    log = prepare(scope)
    marker = scope.parent / 'manifest-edit-done'
    temporary = scope.parent / 'manifest-replacement'
    manifest = scope / 'build.ycxx'
    script = scope / 'rules/check-mode.sh'
    comment = '# Changed during the first witness.'
    # Rename a complete definition while the witness runs, preserving the authored recipe.
    edit = f'''\
if [ ! -e {shlex.quote(str(marker))} ]; then
    /bin/cat {shlex.quote(str(manifest))} > {shlex.quote(str(temporary))} || exit 9
    printf '%s\\n' {shlex.quote(comment)} >> {shlex.quote(str(temporary))} || exit 9
    /bin/mv -f {shlex.quote(str(temporary))} {shlex.quote(str(manifest))} || exit 9
    : > {shlex.quote(str(marker))}
fi
'''
    source = script.read_text(encoding='utf-8')
    script.write_text(source.replace('#!/bin/sh\n', '#!/bin/sh\n' + edit, 1), encoding='utf-8')
    _, events = traced_build(ycxx, scope, env, 'root definition edited during the witness')
    witnesses = [event for event in events if (event.get('kind'), event.get('key')) == ('RuleWitness', EXPORT)]
    assert len(witnesses) == 2, witnesses
    assert action_keys(events, 'RuleClassifier') == [EXPORT], events
    assert marker.exists() and manifest.read_text(encoding='utf-8').count(comment) == 1
    assert log.read_bytes() == b'enabled\nenabled\n'
    check_program(scope / '.ycxx/out/app', 0)
    inspect(ycxx, scope, env, log, 'supported')


def check_value_binding_rewire(ycxx, scope, env):
    log = prepare(scope)
    _, events = traced_build(ycxx, scope, env, 'build initial package value binding')
    check_observation(events)
    initial = inspect(ycxx, scope, env, log, 'supported')
    executable = scope / '.ycxx/out/app'
    check_program(executable, 0)
    before = scope.parent / 'before-value-rewire.json'
    before.write_text(json.dumps(initial), encoding='utf-8')

    backup = 'wire_backup <b data-value-binding-probe="1">&'
    for path in ('build.ycxx', 'rules/codec.cc'):
        source = (scope / path).read_text(encoding='utf-8')
        assert source.count('"wire"') == 2, source
        replace_source(scope, path, source.replace('"wire"', json.dumps(backup)))

    _, events = traced_build(ycxx, scope, env, 'rewire config.fast to an equal package value')
    classified = action_keys(events, 'RuleClassifier')
    assert classified == [f'{backup}.fast-mode'], classified
    assert not action_keys(events, 'RuleWitness'), 'the rewire changed no witness inputs'
    check_program(executable, 0)
    recorded = log.read_bytes()
    assert recorded == b'enabled\n', recorded
    result = run_client(ycxx, scope, env, 'show', 'app', '--format=json')
    assert result.returncode == 0, result.stderr
    current = json.loads(result.stdout)
    fact(current, 'value-export', backup, 'supported')
    consumer = fact(current, 'capability-input', 'config', 'supported')
    assert f'{backup}.fast-mode' in consumer['origin'], consumer
    old_edge, = initial['dependency_graph']['value_edges']
    spelling = f'value({json.dumps(backup)}, "fast-mode")'
    new_edge = {**dependency_edge(current['definition'], 'config', backup, 'capabilities',
                                  spelling, binding='fast'), 'export_name': 'fast-mode'}
    check_dependency_graph(current, initial['dependency_graph']['edges'], value_edges=[new_edge])

    changes = [{'before': [old_edge], 'after': []}, {'before': [], 'after': [new_edge]}]
    diff = compare_value_bindings(ycxx, scope, env, before, current, changes)
    assert diff['dependency_changes'] == [], diff
    assert all(row[side]['kind'] != 'capability-input' for row in diff['changes']
               for side in ('before', 'after') if row.get(side)), diff
    assert log.read_bytes() == recorded, 'comparison ran the witness'


def main():
    ycxx = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='ycxx-package-probe-') as directory:
        scope = Path(directory).resolve() / 'project'
        log = prepare(scope)
        with tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime:
            env = env_for(runtime)
            try:
                incremental = check_edits(ycxx, scope, env, log)
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')

            edited_scope = scope.with_name('edited-project')
            try:
                check_manifest_edit(ycxx, edited_scope, env)
            finally:
                run_client(ycxx, edited_scope, env, 'ctl', 'shutdown')

            rewired_scope = scope.with_name('rewired-project')
            try:
                check_value_binding_rewire(ycxx, rewired_scope, env)
            finally:
                run_client(ycxx, rewired_scope, env, 'ctl', 'shutdown')

        shutil.rmtree(scope / '.ycxx')
        with tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime:
            env = env_for(runtime)
            try:
                _, events = traced_build(ycxx, scope, env, 'cold build with final evidence')
                check_observation(events)
                check_generation(events)
                executable = scope / '.ycxx/out/app'
                assert executable.read_bytes() == incremental, 'cold and incremental executable bytes differ'
                check_program(executable, 20)
                check_invalid_classifier(ycxx, scope, env, log, executable, incremental)
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
