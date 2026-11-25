"""Build, alternate, and watch resident project target configurations (#1345)."""

import json
from pathlib import Path
from struct import unpack_from
import sys
import tempfile

from project_build import action_keys, check_program, daemon_identity, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_inspection import fact
from project_shader_bundle import check_external_actions, publication
from runtime_dir import runtime_root


LINUX = 'x86_64-pc-linux-gnu'
LINUX_ALIAS = 'x86_64-linux-gnu'
DARWIN = 'arm64-apple-darwin'
SUPPORTED = (LINUX, DARWIN)
SCHEMA = 'score.txt'
ASSET = 'assets/app/score-note.txt'
SOURCE = 'main.cc'
REJECTED = 'score generator rejected the input'
RAN = {'executed', 'cutoff', 'recomputed', 'failed'}
GENERATOR = f'''\
IFS= read -r score < "$1" || exit 1
printf '%s\\n' "$score" >> "$2" || exit 1
if [ "$score" = rejected ]; then
    printf '%s\\n' '{REJECTED}' >&2
    exit 7
fi
printf '#define SCORE %s\\n' "$score" > "$3" || exit 1
printf '%s\\n' "$score" > "$4"
'''


def prepare(scope, journal):
    (scope / 'build.ycxx').write_text(f'''\
generate "score" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(GENERATOR)}, "score", input("schema"), {json.dumps(str(journal))},
            output("header"), output("note")]
    inputs = {{ schema = "score.txt" }}
    outputs = {{ header = "score.h", note = "score-note.txt" }}
}}
cpp.executable "app" {{
    sources = ["main.cc"]
    generated_headers = [artifact("score", "header")]
    runtime_assets = [artifact("score", "note")]
}}
''', encoding='utf-8')
    (scope / SOURCE).write_text('''\
#include "score.h"
int main() { return SCORE; }
''', encoding='utf-8')
    (scope / SCHEMA).write_text('17\n', encoding='utf-8')


def namespace(triple):
    return Path('.ycxx/targets') / triple if triple else Path('.ycxx')


def executable(triple):
    return namespace(triple) / 'out/app'


def journal_contents(journal):
    return journal.read_text(encoding='utf-8') if journal.exists() else ''


def target_args(selector, *, separate=False):
    if selector is None:
        return ()
    return ('--target', selector) if separate else (f'--target={selector}',)


def inspect(ycxx, scope, env, journal, selector, canonical, *, cold=False, separate=False):
    before = journal_contents(journal)
    result = run_client(ycxx, scope, env, 'show', 'app', *target_args(selector, separate=separate), '--format=json')
    assert result.returncode == 0, (selector, result.stderr)
    plan = json.loads(result.stdout)
    assert plan['schema'] == 1 and plan['target'] == 'app', plan
    rows = [row for row in plan['facts'] if row['kind'] == 'native-target']
    assert len(rows) == 1, rows
    row = rows[0]
    origin = '--target' if selector else 'host default'
    assert (row['target'], row['name'], row['value'], row['origin']) == ('app', 'triple', canonical, origin), row
    if cold:
        assert any(row['kind'] == 'unresolved' and row['target'] == 'app' for row in plan['facts']), plan
        assert not (scope / '.ycxx').exists(), 'cold show created build state'
    else:
        selected = canonical if selector else None
        asset = fact(plan, 'runtime-asset', 'app', str(namespace(selected) / ASSET))
        assert asset['name'] == 'score-note.txt' and asset['origin'] == 'artifact("score", "note")', asset
        include = fact(plan, 'include', 'app', str(namespace(selected) / 'virtual/score'))
        assert include['origin'] == 'artifact("score", "header")', include
    assert journal_contents(journal) == before, 'show ran the generator'


def check_target_trace(events, canonical, *, explicit=True):
    targets = [(event['key'], event.get('value')) for event in events if event.get('kind') == 'ProjectTarget']
    assert targets == [(canonical, '--target' if explicit else 'host default')], targets


def check_native_reused(events):
    links = {event['outcome'] for event in events if event.get('kind') == 'Link'}
    assert not RAN & links, links
    assert not RAN & tu_outcomes(events, SOURCE), events


def check_binary(scope, selected, canonical, status, host):
    path = scope / executable(selected)
    with path.open('rb') as stream:
        header = stream.read(20)
    if canonical == LINUX:
        assert header[:6] == b'\x7fELF\x02\x01', header
        assert unpack_from('<HH', header, 16) == (2, 62), header  # ET_EXEC, EM_X86_64
    else:
        magic, cpu, _, filetype = unpack_from('<IIII', header)
        assert (magic, cpu, filetype) == (0xFEEDFACF, 0x0100000C, 2), header  # MH_EXECUTE, ARM64
    # PARITY: by design: run only the executable format supported by the current host kernel.
    if canonical == host:
        check_program(path, status)
    note = (scope / namespace(selected) / ASSET).read_text(encoding='utf-8')
    assert note == f'{status}\n', (path, note)


def published(scope, selected):
    return publication(scope / executable(selected)), publication(scope / namespace(selected) / ASSET)


def build(ycxx, scope, env, selector, canonical, label, *, separate=False):
    summary, events = traced_build(ycxx, scope, env, label, *target_args(selector, separate=separate),
                                   output=executable(canonical if selector else None))
    check_target_trace(events, canonical, explicit=selector is not None)
    return summary, events


def check_watch(ycxx, scope, env, installed, host, other):
    watch = Watch(ycxx, scope, env, *target_args(other))
    try:
        summary, events, watched = watch.build('watch the explicitly selected nonhost target')
        check_target_trace(events, other)
        assert summary['actions_executed'] == 0, summary
        assert (scope / summary['output']).resolve() == scope / executable(other), summary
        assert {str(scope / path) for path in (SCHEMA, SOURCE)} <= set(watched), watched
        check_native_reused(events)

        replace_source(scope, SCHEMA, 'rejected\n')
        summary, events, watched = watch.build('explicit watch generator fails', succeeds=False)
        check_target_trace(events, other)
        assert summary['actions_executed'] == 1, summary
        check_external_actions(events, failed=('score',))
        assert any(REJECTED in diagnostic for diagnostic in summary['diagnostics']), summary
        assert str(scope / SCHEMA) in watched, watched
        for selected, outputs in installed.items():
            assert published(scope, selected) == outputs, selected

        replace_source(scope, SCHEMA, '23\n')
        summary, events, _ = watch.build('explicit watch recovers into its own namespace')
        check_target_trace(events, other)
        assert summary['actions_executed'] == 1, summary
        check_external_actions(events, 'score')
        assert (scope / summary['output']).resolve() == scope / executable(other), summary
        assert 'executed' in tu_outcomes(events, SOURCE) and action_keys(events, 'Link'), events
        check_binary(scope, other, other, 23, host)
        current_image, _ = published(scope, other)
        previous_image, _ = installed[other]
        assert current_image[0] != previous_image[0], 'watch kept the old executable bytes'
        for selected in (None, host):
            assert published(scope, selected) == installed[selected], selected
            check_binary(scope, selected, host, 17, host)
    finally:
        watch.close()

    summary, events = build(ycxx, scope, env, host, host, 'host target uses the receipt admitted by nonhost watch')
    assert summary['actions_executed'] == 0, summary
    check_external_actions(events)
    check_binary(scope, host, host, 23, host)
    assert published(scope, None) == installed[None]


def check_builds(ycxx, scope, env, journal, host):
    other, = (triple for triple in SUPPORTED if triple != host)
    for selector, canonical in ((LINUX, LINUX), (DARWIN, DARWIN), (None, host)):
        inspect(ycxx, scope, env, journal, selector, canonical, cold=True)

    _, events = build(ycxx, scope, env, None, host, 'cold host-default build')
    check_external_actions(events, 'score')
    assert 'executed' in tu_outcomes(events, SOURCE) and action_keys(events, 'Link'), events
    check_binary(scope, None, host, 17, host)
    daemon = daemon_identity(ycxx, scope, env)
    assert daemon is not None and daemon[0] == str(scope), daemon

    for selected in SUPPORTED:
        summary, events = build(ycxx, scope, env, selected, selected, 'build explicit target')
        assert summary['actions_executed'] == 0, summary
        check_external_actions(events)
        if selected == other:
            assert 'executed' in tu_outcomes(events, SOURCE) and action_keys(events, 'Link'), events
        check_binary(scope, selected, selected, 17, host)
    assert journal_contents(journal) == '17\n', 'target selection reran the host generator'
    installed = {selected: published(scope, selected) for selected in (None, *SUPPORTED)}

    for selector, canonical, separate in (
        (LINUX_ALIAS, LINUX, False), (DARWIN, DARWIN, True), (LINUX, LINUX, False), (None, host, False),
    ):
        summary, events = build(ycxx, scope, env, selector, canonical, 'alternate warm configurations', separate=separate)
        assert summary['actions_executed'] == 0, summary
        check_external_actions(events)
        check_native_reused(events)
        inspect(ycxx, scope, env, journal, selector, canonical, separate=separate)
    assert daemon_identity(ycxx, scope, env) == daemon
    assert not (scope / namespace(LINUX_ALIAS)).exists(), 'target alias created its own output namespace'
    for selected, outputs in installed.items():
        assert published(scope, selected) == outputs, selected

    check_watch(ycxx, scope, env, installed, host, other)
    assert journal_contents(journal) == '17\nrejected\n23\n', 'generation was not shared across targets'
    assert daemon_identity(ycxx, scope, env) == daemon


def main():
    ycxx = Path(sys.argv[1]).resolve()
    host = {'linux': LINUX, 'darwin': DARWIN}[sys.platform]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-project-targets-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        scope = root / 'project'
        scope.mkdir()
        journal = root / 'score-runs'
        env = env_for(runtime)
        prepare(scope, journal)
        try:
            check_builds(ycxx, scope, env, journal, host)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
