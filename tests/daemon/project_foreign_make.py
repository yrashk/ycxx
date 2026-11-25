"""Exercise the foreign Make action and its native archive consumer (#1341)."""

import html
import json
from pathlib import Path
import shlex
import shutil
import sys
import tempfile

from project_build import action_keys, check_program, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_inspection import check_location, fact
from project_shader_bundle import check_external_actions, publication
from runtime_dir import runtime_root


EXAMPLE = Path(__file__).resolve().parents[2] / 'examples/project-build/foreign-make'
PRODUCER = 'foreign_score'
INVENTORY = 'vendor/files.txt'
MAKEFILE = 'vendor/Makefile'
SOURCE = 'vendor/src/score.c'
HEADER = 'vendor/include/score.h'
CONFIG = 'vendor/include/score_config.h'
ARCHIVE = '.ycxx/virtual/foreign_score/libforeign.a'
REFERENCE = 'artifact("foreign_score", "archive")'
NATIVE = '.ycxx/out/app'
NATIVE_SOURCES = ('main.cc', 'bridge.cc')
RAN = {'executed', 'cutoff', 'recomputed', 'failed'}
REJECT = 'bad vendor edit'


def prepare(scope, journal):
    shutil.copytree(EXAMPLE, scope, ignore=shutil.ignore_patterns('.ycxx'))
    # Count actual Make invocations without changing its arguments, environment, or cwd.
    tool = scope.parent / 'make-fixture'
    tool.write_text(f'#!/bin/sh\nprintf "%s\\n" make >> {shlex.quote(str(journal))} || exit 1\n'
                    'exec /usr/bin/make "$@"\n', encoding='utf-8')
    tool.chmod(0o700)
    manifest = scope / 'build.ycxx'
    source = manifest.read_text(encoding='utf-8')
    assert source.count('"/usr/bin/make"') == 1, source
    manifest.write_text(source.replace('"/usr/bin/make"', json.dumps(str(tool))), encoding='utf-8')
    return tool


def journal_bytes(journal):
    return journal.read_bytes() if journal.exists() else b''


def inspect(ycxx, scope, env, journal, tool, *, cold=False, inventory_files=(SOURCE, HEADER, CONFIG)):
    before = journal_bytes(journal)
    result = run_client(ycxx, scope, env, 'show', 'app', '--format=json')
    assert result.returncode == 0, result.stderr
    plan = json.loads(result.stdout)
    assert plan['schema'] == 1 and plan['target'] == 'app', plan
    if cold:
        assert any(row['kind'] == 'unresolved' and row['target'] == 'app' for row in plan['facts']), plan
        assert not (scope / '.ycxx').exists(), 'cold show created build state'
    else:
        fact(plan, 'tool', PRODUCER, str(tool))
        inputs = {row['name']: row['value'] for row in plan['facts']
                  if row['kind'] == 'input' and row['target'] == PRODUCER}
        staged = {f'file:{path.removeprefix("vendor/")}': path for path in inventory_files}
        assert inputs == {'makefile': MAKEFILE, **staged}, inputs
        for index, argument in enumerate(('-f', f'inputs/{MAKEFILE}', 'archive')):
            assert fact(plan, 'argv', PRODUCER, argument)['name'] == str(index), plan
        assert fact(plan, 'output', PRODUCER, ARCHIVE)['name'] == 'archive', plan
        archive = fact(plan, 'link-input', 'app', ARCHIVE)
        assert archive['name'] == '0', archive
        assert all(part in archive['origin'] for part in ('inherited from bridge', 'public_link_inputs', REFERENCE)), archive
        check_location(archive, plan['definition'], REFERENCE)

        result = run_client(ycxx, scope, env, 'show', 'app', '--format=html')
        assert result.returncode == 0, result.stderr
        document = html.unescape(result.stdout)
        assert all(part in document for part in (f'inputs/{MAKEFILE}', *inventory_files, ARCHIVE,
                                                  'inherited from bridge')), document
    assert journal_bytes(journal) == before, 'show ran Make'


def check_native(events, *, linked=False):
    for source in NATIVE_SOURCES:
        outcomes = tu_outcomes(events, source)
        assert not outcomes & RAN, (source, outcomes)
    if linked:
        assert action_keys(events, 'Link'), events
    else:
        outcomes = {event['outcome'] for event in events if event.get('kind') == 'Link'}
        assert not outcomes & RAN, outcomes


def check_make(summary, events, *, failed=False, lowered=False):
    assert summary['actions_executed'] == 1 + lowered, summary
    if failed:
        check_external_actions(events, failed=(PRODUCER,))
    else:
        check_external_actions(events, PRODUCER)
    assert action_keys(events, 'RuleWorker') == ([PRODUCER] if lowered else []), events


def check_watch(ycxx, scope, env, journal, tool):
    executable = scope / NATIVE
    config = (scope / CONFIG).read_text(encoding='utf-8')
    source = (scope / SOURCE).read_text(encoding='utf-8')
    inventory = (scope / INVENTORY).read_text(encoding='utf-8')
    assert config.count('#define SCORE_BONUS 7') == 1, config
    config8 = config.replace('#define SCORE_BONUS 7', '#define SCORE_BONUS 8')
    config9 = config.replace('#define SCORE_BONUS 7', '#define SCORE_BONUS 9')
    paths = {str(scope / path) for path in (INVENTORY, MAKEFILE, SOURCE, HEADER, CONFIG)}
    watch = Watch(ycxx, scope, env)
    try:
        summary, events, watched = watch.build('watch the current foreign archive')
        assert summary['actions_executed'] == 0, summary
        assert paths <= set(watched), watched
        check_native(events)

        replace_source(scope, CONFIG, config8)
        summary, events, _ = watch.build('foreign config change relinks the app')
        check_make(summary, events)
        check_native(events, linked=True)
        check_program(executable, 43)
        installed = publication(executable)

        replace_source(scope, SOURCE, source + f'\n#error {REJECT}\n')
        summary, events, watched = watch.build('vendor compilation fails', succeeds=False)
        check_make(summary, events, failed=True)
        check_native(events)
        assert any(REJECT in diagnostic for diagnostic in summary['diagnostics']), summary
        assert paths <= set(watched), watched
        assert publication(executable) == installed, 'failed Make replaced the executable'
        check_program(executable, 43)

        replace_source(scope, SOURCE, source + '\n/* Repaired vendor source. */\n')
        summary, events, _ = watch.build('watch recovers with the corrected source')
        check_make(summary, events)
        check_native(events)
        assert publication(executable) == installed
        check_program(executable, 43)

        replace_source(scope, CONFIG, config9)
        summary, events, _ = watch.build('second foreign config change relinks the app')
        check_make(summary, events)
        check_native(events, linked=True)
        check_program(executable, 44)
        installed = publication(executable)

        entry = 'include/score_config.h'
        assert inventory.splitlines().count(entry) == 1, inventory
        replace_source(scope, INVENTORY, ''.join(line for line in inventory.splitlines(keepends=True)
                                                if line.rstrip('\r\n') != entry))
        summary, events, watched = watch.build('undeclared header is absent from the Make inputs', succeeds=False)
        check_make(summary, events, failed=True, lowered=True)
        check_native(events)
        assert any(entry in diagnostic for diagnostic in summary['diagnostics']), summary
        assert str(scope / INVENTORY) in watched, watched
        assert (scope / CONFIG).read_text(encoding='utf-8') == config9, 'the omitted header was deleted'
        assert publication(executable) == installed
        check_program(executable, 44)
        inspect(ycxx, scope, env, journal, tool, inventory_files=(SOURCE, HEADER))

        replace_source(scope, INVENTORY, inventory)
        summary, events, _ = watch.build('restore the declaration and reuse the last good archive')
        assert summary['actions_executed'] == 0, summary
        check_native(events)
        assert publication(executable) == installed
    finally:
        watch.close()


def check_builds(ycxx, scope, env, journal, tool):
    vendor = scope / 'vendor'
    vendor_files = {path.relative_to(vendor) for path in vendor.rglob('*') if path.is_file()}
    inspect(ycxx, scope, env, journal, tool, cold=True)

    _, events = traced_build(ycxx, scope, env, 'cold foreign archive and native app')
    check_external_actions(events, PRODUCER)
    assert PRODUCER in action_keys(events, 'RuleWorker'), events
    assert all('executed' in tu_outcomes(events, source) for source in NATIVE_SOURCES), events
    assert action_keys(events, 'Link'), events
    assert journal_bytes(journal) == b'make\n', 'cold build did not run exactly one Make invocation'
    executable = scope / NATIVE
    check_program(executable, 42)
    installed = publication(executable)
    inspect(ycxx, scope, env, journal, tool)

    summary, events = traced_build(ycxx, scope, env, 'unchanged foreign project')
    assert summary['actions_executed'] == 0, summary
    check_native(events)
    assert publication(executable) == installed

    original = (scope / SOURCE).read_text(encoding='utf-8')
    replace_source(scope, SOURCE, original + '\n/* Comment only, archive bytes stay equal. */\n')
    inspect(ycxx, scope, env, journal, tool)
    summary, events = traced_build(ycxx, scope, env, 'equal archive bytes after a vendor source edit')
    check_make(summary, events)
    check_native(events)
    assert journal_bytes(journal) == b'make\nmake\n', 'only the foreign action should have run'
    assert publication(executable) == installed

    check_watch(ycxx, scope, env, journal, tool)
    inspect(ycxx, scope, env, journal, tool)
    assert {path.relative_to(vendor) for path in vendor.rglob('*') if path.is_file()} == vendor_files


def main():
    ycxx = Path(sys.argv[1]).resolve()
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-foreign-make-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        scope = root / 'project'
        journal = root / 'make-runs'
        env = env_for(runtime)
        tool = prepare(scope, journal)
        try:
            check_builds(ycxx, scope, env, journal, tool)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
