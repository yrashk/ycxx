"""Exercise ordered generated archives inherited through native project libraries (#1339)."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile

from project_build import action_keys, check_program, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_shader_bundle import check_external_actions, publication
from runtime_dir import runtime_root


NATIVE = '.ycxx/out/app'
NOTE = '.ycxx/assets/app/selection.txt'
SELECTOR = 'selection.txt'
SOURCES = ('main.cc', 'middle.cc', 'leaf.cc')
ORDER = ('z_provider', 'a_provider', 'z_provider')
REVERSED_PROVIDER = ('a_provider', 'z_provider', 'a_provider')
RAN = {'executed', 'cutoff', 'recomputed', 'failed'}
SELECT = r'''
printf 'generated\n' >> "$1" || exit 1
IFS= read -r selection < "$2" || exit 1
case "$selection" in
    one) /bin/cat "$3" > "$6" ;;
    two) /bin/cat "$4" > "$6" ;;
    three) /bin/cat "$5" > "$6" ;;
    malformed) printf '!<arch>\nshort' > "$6" ;;
    *) exit 1 ;;
esac || exit 1
printf '%s\n' "$selection" > "$7"
'''
DIAMOND = '''\
cpp.library "leaf" {
    sources = ["leaf.cc"]
    deps = ["left", "right"]
}
cpp.library "left" {
    deps = ["common"]
    public_link_inputs = [artifact("z_provider", "archive")]
}
cpp.library "right" {
    deps = ["common"]
    public_link_inputs = [artifact("a_provider", "archive")]
}
cpp.library "common" {
    public_link_inputs = [artifact("z_provider", "archive")]
}
'''


def manifest(journal, providers, *, diamond=False, direct=None):
    references = ',\n'.join(f'        artifact("{target}", "archive")' for target in providers)
    libraries = DIAMOND if diamond else f'''\
cpp.library "leaf" {{
    sources = ["leaf.cc"]
    public_link_inputs = [
{references}
    ]
}}
'''
    direct_link = f'    link_inputs = [artifact("{direct}", "archive")]\n' if direct else ''
    return f'''\
generate "z_provider" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(SELECT)}, "select-archive", {json.dumps(str(journal))},
            input("selector"), input("one"), input("two"), input("three"),
            output("archive"), output("note")]
    inputs = {{ selector = "selection.txt", one = "archives/one.a",
                two = "archives/two.a", three = "archives/three.a" }}
    outputs = {{ archive = "provider.a", note = "selection.txt" }}
}}
generate "a_provider" {{
    tool = "/bin/cp"
    args = [input("archive"), output("archive")]
    inputs = {{ archive = "archives/alternative.a" }}
    outputs = {{ archive = "provider.a" }}
}}
{libraries}cpp.library "middle" {{
    sources = ["middle.cc"]
    deps = ["leaf"]
}}
cpp.executable "app" {{
    sources = ["main.cc"]
    deps = ["middle"]
{direct_link}    runtime_assets = [artifact("z_provider", "note")]
}}
'''


def checked_command(*args, cwd, env):
    result = subprocess.run(args, cwd=cwd, env=env, capture_output=True, timeout=30)
    assert result.returncode == 0, (args, result.stdout, result.stderr)


def prepare(ycxx, archiver, scope, env, journal):
    (scope / 'archives').mkdir(parents=True)
    (scope / 'build.ycxx').write_text(manifest(journal, ORDER), encoding='utf-8')
    (scope / SELECTOR).write_text('one\n', encoding='utf-8')
    for source, text in (
        ('main.cc', 'int middle(); int main() { return middle(); }\n'),
        ('middle.cc', 'int leaf(); int middle() { return leaf(); }\n'),
        ('leaf.cc', 'extern "C" int choose(); int leaf() { return choose(); }\n'),
    ):
        (scope / source).write_text(text, encoding='utf-8')

    # Compile ordinary native objects and use the host archiver to get its native index format.
    tools = scope.parent / 'archive-members'
    tools.mkdir()
    standalone = dict(env, YCXX_NO_DAEMON='1')
    standalone.pop('YCXX_DAEMON', None)
    # The first provider needs a member from the other archive. On ELF that
    # member's reference to finish() is resolved by the repeated first archive.
    members = {
        'finish': 'extern "C" int finish() { return 1; }',
        'bridge': 'extern "C" int finish(); extern "C" int bridge() { return finish(); }',
        'alternative': 'extern "C" int choose() { return 31; }',
    }
    for name, value in (('one', 16), ('two', 22), ('three', 40)):
        members[name] = f'extern "C" int bridge(); extern "C" int choose() {{ return bridge() + {value}; }}'
    for name, text in members.items():
        (tools / f'{name}.cc').write_text(text + '\n', encoding='utf-8')
        checked_command(ycxx, 'cxx', '-c', f'{name}.cc', '-o', f'{name}.o', cwd=tools, env=standalone)
    for name, other in (('one', 'finish'), ('two', 'finish'), ('three', 'finish'), ('alternative', 'bridge')):
        checked_command(archiver, 'rcs', scope / 'archives' / f'{name}.a', f'{name}.o', f'{other}.o',
                        cwd=tools, env=standalone)


def journal_bytes(journal):
    return journal.read_bytes() if journal.exists() else b''


def inspect(ycxx, scope, env, journal, providers=(), *, owners=None, direct=None):
    before = journal_bytes(journal)
    result = run_client(ycxx, scope, env, 'show', 'app', '--format=json')
    assert result.returncode == 0, result.stderr
    assert journal_bytes(journal) == before, 'show executed an archive producer'
    plan = json.loads(result.stdout)
    assert plan['schema'] == 1 and plan['target'] == 'app', plan
    inputs = sorted((row for row in plan['facts'] if row['kind'] == 'link-input'),
                    key=lambda row: int(row['name']))
    declarations = list(zip(owners or ('leaf',) * len(providers), providers))
    if direct:
        declarations.insert(0, ('app', direct))
    assert len(inputs) == len(declarations), inputs
    if not declarations:
        assert any(row['kind'] == 'unresolved' and row['target'] == 'app' for row in plan['facts']), plan
        return
    definition = plan['definition']
    offsets = {}
    for position, (row, (owner, provider)) in enumerate(zip(inputs, declarations)):
        reference = f'artifact("{provider}", "archive")'
        start = definition.index(reference, offsets.get(owner, definition.index(f'"{owner}" {{')))
        offsets[owner] = start + len(reference)
        location = definition.count('\n', 0, start) + 1, start - definition.rfind('\n', 0, start)
        origin = f'declared in link_inputs as {reference}' if owner == 'app' else (
            f'inherited from {owner} (declared in public_link_inputs as {reference})')
        assert (row['target'], row['name'], row['value']) == (
            'app', str(position), f'.ycxx/virtual/{provider}/provider.a'), row
        assert row['origin'] == origin, row
        assert (row['line'], row['column']) == location, row


def check_native_units_reused(events):
    for source in SOURCES:
        outcomes = tu_outcomes(events, source)
        assert not outcomes & RAN, (source, outcomes)


def check_link_reused(events):
    outcomes = {event['outcome'] for event in events if event.get('kind') == 'Link'}
    assert not outcomes & RAN, outcomes
    check_native_units_reused(events)


def check_note(scope, expected):
    actual = (scope / NOTE).read_text(encoding='utf-8')
    assert actual == f'{expected}\n', (actual, expected)


def check_watch(ycxx, scope, env):
    executable = scope / NATIVE
    note = scope / NOTE
    watch = Watch(ycxx, scope, env)
    try:
        summary, events, watched = watch.build('watch current link inputs')
        assert summary['actions_executed'] == 0, summary
        assert {str(scope / path) for path in (SELECTOR, 'archives/alternative.a')} <= set(watched), watched
        check_link_reused(events)

        replace_source(scope, SELECTOR, 'two\n')
        summary, events, _ = watch.build('new archive bytes relink the existing objects')
        assert summary['actions_executed'] == 1, summary
        check_external_actions(events, 'z_provider')
        assert action_keys(events, 'Link'), events
        check_native_units_reused(events)
        check_program(executable, 23)
        check_note(scope, 'two')
        last_good = publication(executable), publication(note)

        replace_source(scope, SELECTOR, 'malformed\n')
        summary, events, watched = watch.build('malformed archive leaves the last good output', succeeds=False)
        assert summary['actions_executed'] == 1, summary
        check_external_actions(events, 'z_provider')
        assert any('archive' in diagnostic.lower() and 'provider.a' in diagnostic
                   for diagnostic in summary['diagnostics']), summary
        check_native_units_reused(events)
        assert str(scope / SELECTOR) in watched, watched
        assert (publication(executable), publication(note)) == last_good
        check_program(executable, 23)

        replace_source(scope, SELECTOR, 'three\n')
        summary, events, _ = watch.build('watch recovers when the producer returns a valid archive')
        assert summary['actions_executed'] == 1, summary
        check_external_actions(events, 'z_provider')
        assert action_keys(events, 'Link'), events
        check_native_units_reused(events)
        check_program(executable, 41)
        check_note(scope, 'three')
        watch.idle('archive recovery and publication')
    finally:
        watch.close()


def check_builds(ycxx, scope, env, journal):
    inspect(ycxx, scope, env, journal)
    assert not (scope / '.ycxx').exists(), 'cold show created build state'
    _, events = traced_build(ycxx, scope, env, 'cold transitive archive link')
    check_external_actions(events, 'a_provider', 'z_provider')
    assert all('executed' in tu_outcomes(events, source) for source in SOURCES), events
    assert action_keys(events, 'Link'), events
    assert journal_bytes(journal) == b'generated\n', 'the archive and runtime note caused two runs'
    executable = scope / NATIVE
    note = scope / NOTE
    check_program(executable, 17)
    check_note(scope, 'one')
    initial = publication(executable), publication(note)
    inspect(ycxx, scope, env, journal, ORDER)

    summary, events = traced_build(ycxx, scope, env, 'unchanged archive inputs')
    assert summary['actions_executed'] == 0, summary
    check_external_actions(events)
    check_link_reused(events)
    assert (publication(executable), publication(note)) == initial

    replace_source(scope, SELECTOR, 'one\n# comment has no effect on the archive\n')
    inspect(ycxx, scope, env, journal, ORDER)
    summary, events = traced_build(ycxx, scope, env, 'equivalent archive bytes')
    assert summary['actions_executed'] == 1, summary
    check_external_actions(events, 'z_provider')
    check_link_reused(events)
    assert (publication(executable), publication(note)) == initial

    check_watch(ycxx, scope, env)

    replace_source(scope, 'build.ycxx', manifest(journal, REVERSED_PROVIDER))
    _, events = traced_build(ycxx, scope, env, 'prefer the other provider by changing archive order')
    check_external_actions(events)
    assert action_keys(events, 'Link'), events
    check_native_units_reused(events)
    check_program(executable, 31)
    inspect(ycxx, scope, env, journal, REVERSED_PROVIDER)

    replace_source(scope, 'build.ycxx', manifest(journal, ORDER, diamond=True))
    _, events = traced_build(ycxx, scope, env, 'order a shared library after both of its consumers')
    check_external_actions(events)
    check_native_units_reused(events)
    check_program(executable, 41)
    inspect(ycxx, scope, env, journal, ORDER, owners=('left', 'right', 'common'))

    direct = 'a_provider'
    replace_source(scope, 'build.ycxx', manifest(journal, ORDER, diamond=True, direct=direct))
    _, events = traced_build(ycxx, scope, env, 'executable link inputs precede inherited archives')
    check_external_actions(events)
    assert action_keys(events, 'Link'), events
    check_native_units_reused(events)
    check_program(executable, 31)
    inspect(ycxx, scope, env, journal, ORDER, owners=('left', 'right', 'common'), direct=direct)


def main():
    ycxx = Path(sys.argv[1]).resolve()
    archiver = sys.argv[2]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-project-archives-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        scope = root / 'project'
        journal = root / 'archive-runs'
        env = env_for(runtime)
        prepare(ycxx, archiver, scope, env, journal)
        try:
            check_builds(ycxx, scope, env, journal)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
