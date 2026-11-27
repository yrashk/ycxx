"""Run a generator's staged artifact as another generator's tool (#1505)."""

import json
from pathlib import Path
import shlex
import shutil
import stat
import sys
import tempfile

from project_build import action_keys, check_program, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_inspection import (
    check_dependency_graph, check_dependency_html, check_location, check_target_dependencies_html,
    dependency_edge, fact, scope_state,
)
from project_shader_bundle import check_external_actions, check_native_reused, publication
from runtime_dir import runtime_root


EXAMPLE = Path(__file__).resolve().parents[2] / 'examples/project-build/staged-tool'
RUNNER = 'runner.sh'
RULE = 'rules/staged_tool.cc'
UNUSED = 'unused.txt'
NATIVE = '.ycxx/out/app'
ARTIFACT = 'artifact("runner", "runner")'
HEADER = 'artifact("config", "header")'
SELECTOR = 'input("runner")'
TOOL = 'inputs/artifacts/runner/runner.sh'


def prepare(scope, journal):
    shutil.copytree(EXAMPLE, scope, ignore=shutil.ignore_patterns('.ycxx'))
    runner = scope / RUNNER
    assert runner.stat().st_mode & stat.S_IXUSR, 'fixture runner must start executable'
    shebang, script = runner.read_text(encoding='utf-8').split('\n', 1)
    runner.write_text(f"{shebang}\nprintf '%s\\n' ran >> {shlex.quote(str(journal))}\n{script}", encoding='utf-8')

    # Changing this declared input reruns the producer with the same executable bytes and mode.
    rule = scope / RULE
    source = rule.read_text(encoding='utf-8')
    inputs = 'inputs = { script = "runner.sh" }'
    assert source.count(inputs) == 1, source
    rule.write_text(source.replace(inputs, 'inputs = { script = "runner.sh" unused = "unused.txt" }'),
                    encoding='utf-8')
    (scope / UNUSED).write_text('initial evidence\n', encoding='utf-8')


def journal_bytes(journal):
    return journal.read_bytes() if journal.exists() else b''


def inspect(ycxx, scope, env, journal, *, cold=False):
    before = scope_state(scope)
    recorded = journal_bytes(journal)
    result = run_client(ycxx, scope, env, 'show', 'app', '--format=json')
    assert result.returncode == 0, result.stderr
    plan = json.loads(result.stdout)
    assert plan['schema'] == 1 and plan['target'] == 'app', plan
    if cold:
        check_dependency_graph(plan, [], complete=False)
        assert any(row['kind'] == 'unresolved' and row['target'] == 'app' for row in plan['facts']), plan
    else:
        tool = fact(plan, 'tool', 'config', TOOL)
        assert (tool['name'], tool['origin']) == ('tool', SELECTOR), tool
        definition = plan['definition']
        check_location(tool, definition, 'tools.header "config"')
        assert fact(plan, 'input', 'config', TOOL.removeprefix('inputs/'))['origin'] == ARTIFACT
        check_dependency_graph(plan, [
            dependency_edge(definition, 'app', 'config', 'generated_headers', HEADER, output='header'),
            dependency_edge(definition, 'config', 'runner', 'inputs', 'tools.header "config"',
                            binding='runner', output='runner'),
        ])

        result = run_client(ycxx, scope, env, 'show', 'app', '--format=html')
        assert result.returncode == 0, result.stderr
        assert TOOL in result.stdout, 'missing staged tool path in HTML'
        check_dependency_html(result.stdout, plan)
        check_target_dependencies_html(result.stdout, plan)
    assert journal_bytes(journal) == recorded and scope_state(scope) == before, 'inspection changed the workspace'


def check_watch(ycxx, scope, env, journal, original, installed):
    runner = scope / RUNNER
    executable = scope / NATIVE
    mode = stat.S_IMODE(runner.stat().st_mode)
    watch = Watch(ycxx, scope, env)
    try:
        summary, events, watched = watch.build('unchanged staged tool under watch')
        assert {str(runner), str(scope / UNUSED)} <= set(watched), watched
        assert summary['actions_executed'] == 0, summary
        check_external_actions(events)
        check_native_reused(events, executable, installed)

        replace_source(scope, UNUSED, 'different evidence, same runner\n')
        _, events, _ = watch.build('regenerate equal runner bytes and mode')
        check_external_actions(events, 'runner')
        check_native_reused(events, executable, installed)
        assert journal_bytes(journal) == b'ran\n', 'equal runner receipt relaunched the staged tool'

        assert original.count('generated_score = 7;') == 1, original
        changed = original.replace('generated_score = 7;', 'generated_score = 8;')
        pending = scope / '.ycxx' / 'watch-new-runner'
        pending.write_text(changed, encoding='utf-8')
        pending.chmod(mode)
        pending.replace(runner)
        _, events, watched = watch.build('change the staged runner bytes')
        assert str(runner) in watched, watched
        check_external_actions(events, 'runner', 'config')
        assert 'executed' in tu_outcomes(events, 'main.cc') and action_keys(events, 'Link'), events
        check_program(executable, 8)
        published = publication(executable)
        assert published[0] != installed[0], 'runner edit kept stale native bytes'
        assert journal_bytes(journal) == b'ran\nran\n'

        runner.chmod(mode & ~0o111)
        summary, events, watched = watch.build('reject the same runner bytes without execute permission', succeeds=False)
        assert str(runner) in watched, watched
        assert any('not executable' in item and TOOL in item for item in summary['diagnostics']), summary
        check_external_actions(events, 'runner', failed=('config',))
        check_native_reused(events, executable, published)
        assert runner.read_text(encoding='utf-8') == changed
        assert journal_bytes(journal) == b'ran\nran\n', 'non-executable runner was launched'
        check_program(executable, 8)
        inspect(ycxx, scope, env, journal)

        runner.chmod(mode)
        summary, events, _ = watch.build('restore the executable mode')
        assert summary['actions_executed'] == 0, summary
        check_external_actions(events)
        check_native_reused(events, executable, published)
        assert journal_bytes(journal) == b'ran\nran\n', 'restoring the admitted input relaunched the staged tool'
        check_program(executable, 8)
        watch.idle('recovery with an admitted staged tool')
    finally:
        watch.close()


def check_rejected_selectors(ycxx, scope, env, journal):
    original = (scope / RULE).read_text(encoding='utf-8')
    assert original.count(f'tool = {SELECTOR}') == 1, original
    assert original.count(f'runner = {ARTIFACT}') == 1, original
    executable = scope / NATIVE
    installed = publication(executable)
    recorded = journal_bytes(journal)
    for binding, source in (
        ('missing', original.replace(f'tool = {SELECTOR}', 'tool = input("missing")')),
        ('runner', original.replace(f'runner = {ARTIFACT}', 'runner = "runner.sh"')),
    ):
        replace_source(scope, RULE, source)
        summary, events = traced_build(ycxx, scope, env, f'reject staged tool binding {binding}', succeeds=False)
        assert any(f"tool input '{binding}' must name an artifact-backed input" in item
                   for item in summary['diagnostics']), summary
        check_external_actions(events)
        check_native_reused(events, executable, installed)
        assert journal_bytes(journal) == recorded, 'invalid selector launched a staged tool'


def check_builds(ycxx, scope, env, journal):
    executable = scope / NATIVE
    inspect(ycxx, scope, env, journal, cold=True)
    assert not (scope / '.ycxx').exists() and not journal.exists(), 'cold inspection ran the project'

    # Admit both generator plans while deliberately stopping before native publication.
    main = scope / 'main.cc'
    source = main.read_text(encoding='utf-8')
    main.write_text(source + '\n#error stop before publication\n', encoding='utf-8')
    summary, events = traced_build(ycxx, scope, env, 'warm tool receipts before publication', succeeds=False)
    assert any('stop before publication' in item for item in summary['diagnostics']), summary
    check_external_actions(events, 'runner', 'config')
    assert not executable.exists()
    assert journal_bytes(journal) == b'ran\n'
    inspect(ycxx, scope, env, journal)
    assert not executable.exists(), 'inspection published the native executable'

    replace_source(scope, 'main.cc', source)
    _, events = traced_build(ycxx, scope, env, 'build native app with the admitted staged tool')
    check_external_actions(events)
    assert 'executed' in tu_outcomes(events, 'main.cc') and action_keys(events, 'Link'), events
    check_program(executable, 7)
    installed = publication(executable)
    check_watch(ycxx, scope, env, journal, (scope / RUNNER).read_text(encoding='utf-8'), installed)
    check_rejected_selectors(ycxx, scope, env, journal)


def main():
    ycxx = Path(sys.argv[1]).resolve()
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-staged-tool-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        scope, journal = root / 'project', root / 'runner-runs'
        env = env_for(runtime)
        prepare(scope, journal)
        try:
            check_builds(ycxx, scope, env, journal)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
