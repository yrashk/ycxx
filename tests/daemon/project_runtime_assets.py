"""Runtime asset ownership and publication through the resident daemon (#1329)."""

import json
from pathlib import Path
import sys
import tempfile

from project_build import action_keys, check_program, daemon_identity, env_for, run_client, traced_build
from project_build_watch import Watch, replace_source
from runtime_dir import runtime_root


GENERATOR = r'''
IFS= read -r value < "$1" || exit 5
printf 'primary %s\n' "$value" > "$2" || exit 5
if [ "$value" = rejected ]; then
    printf 'asset input rejected\n' >&2
    exit 7
fi
printf 'secondary %s\n' "$value" > "$3"
'''


def manifest(filename, *, secondary=False):
    assets = ''
    if filename is not None:
        selected = ['artifact("pack", "primary")']
        if secondary:
            selected.append('artifact("pack", "secondary")')
        assets = f'    runtime_assets = [{", ".join(selected)}]\n'
    return f'''\
generate "pack" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(GENERATOR)}, "generator",
            input("data"), output("primary"), output("secondary")]
    inputs = {{ data = "assets.txt" }}
    outputs = {{ primary = {json.dumps(filename or 'renamed.bin')} secondary = "obsolete.bin" }}
}}
cpp.executable "app" {{
    sources = ["main.cc"]
{assets}}}
'''


def check_assets(directory, expected, *, absent=()):
    assert directory.is_dir() and not directory.is_symlink(), directory
    status = directory.stat()
    stamps = [(status.st_dev, status.st_ino)]
    for name, contents in expected.items():
        path = directory / name
        assert path.is_file() and not path.is_symlink(), path
        actual = path.read_bytes()
        assert actual == contents, (name, actual, contents)
        status = path.stat()
        stamps.append((status.st_dev, status.st_ino, status.st_mtime_ns))
    for name in absent:
        path = directory / name
        assert not path.exists() and not path.is_symlink(), ('obsolete asset still published', path)
    return stamps


def check_foreign(directory, outside):
    assert (directory / 'foreign.bin').read_bytes() == b'foreign data\n'
    assert (directory / 'outside-link').readlink() == outside
    assert outside.read_bytes() == b'outside the workspace\n'


def check_watch(scope, watch, directory, executable, outside):
    watched = watch.build('initial two-asset generation')[2]
    assert {'build.ycxx', 'assets.txt', 'main.cc'} <= {Path(path).name for path in watched}, watched
    check_program(executable, 3)
    initial = {'old.bin': b'primary alpha\n', 'obsolete.bin': b'secondary alpha\n'}
    previous = check_assets(directory, initial, absent=('renamed.bin',))
    (directory / 'foreign.bin').write_bytes(b'foreign data\n')
    (directory / 'outside-link').symlink_to(outside)

    replace_source(scope, 'assets.txt', 'rejected\n')
    summary, events, _ = watch.build('partially written tool output', succeeds=False)
    assert action_keys(events, 'ExternalAction', 'failed') == ['pack'], events
    assert any('asset input rejected' in diagnostic for diagnostic in summary['diagnostics']), summary
    assert check_assets(directory, initial, absent=('renamed.bin',)) == previous
    check_program(executable, 3)

    replace_source(scope, 'assets.txt', 'beta\n')
    watch.build('replace both current assets')
    updated = {'old.bin': b'primary beta\n', 'obsolete.bin': b'secondary beta\n'}
    previous = check_assets(directory, updated, absent=('renamed.bin',))

    # The output destination becomes unpublishable before the watched plan changes.
    # A complete new asset set is available, but the old directory must stay live.
    saved = executable.with_name('app-saved')
    executable.rename(saved)
    executable.mkdir()
    blocker = executable / 'keep'
    blocker.touch()
    replace_source(scope, 'build.ycxx', manifest('renamed.bin'))
    summary, events, _ = watch.build('blocked executable publication', succeeds=False)
    assert action_keys(events, 'ExternalAction') == ['pack'], events
    assert any('.ycxx/out/app' in diagnostic for diagnostic in summary['diagnostics']), summary
    assert check_assets(directory, updated, absent=('renamed.bin',)) == previous
    check_foreign(directory, outside)
    blocker.unlink()
    executable.rmdir()
    saved.rename(executable)

    replace_source(scope, 'main.cc', '#error native fixture rejected\n')
    summary, _, _ = watch.build('native build fails with a new asset plan', succeeds=False)
    assert any('native fixture rejected' in diagnostic for diagnostic in summary['diagnostics']), summary
    assert check_assets(directory, updated, absent=('renamed.bin',)) == previous
    check_program(executable, 3)

    replace_source(scope, 'main.cc', 'int main() { return 4; }\n')
    watch.build('publish renamed asset and remove obsolete assets')
    check_program(executable, 4)
    check_assets(directory, {'renamed.bin': b'primary beta\n'}, absent=tuple(updated))
    check_foreign(directory, outside)
    watch.idle('publication does not trigger another build')


def main():
    ycxx = Path(sys.argv[1]).resolve()
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-runtime-assets-') as workspace,
        tempfile.TemporaryDirectory(prefix='ycxx-asset-outside-') as outside_dir,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as restarted_runtime,
    ):
        scope = Path(workspace).resolve()
        directory = scope / '.ycxx' / 'assets' / 'app'
        executable = scope / '.ycxx' / 'out' / 'app'
        outside = Path(outside_dir).resolve() / 'sentinel'
        outside.write_bytes(b'outside the workspace\n')
        (scope / 'build.ycxx').write_text(manifest('old.bin', secondary=True), encoding='utf-8')
        (scope / 'main.cc').write_text('int main() { return 3; }\n', encoding='utf-8')
        (scope / 'assets.txt').write_text('alpha\n', encoding='utf-8')

        env = env_for(runtime)
        expected = {'renamed.bin': b'primary beta\n'}
        old_names = ('old.bin', 'obsolete.bin')
        try:
            watch = Watch(ycxx, scope, env)
            try:
                check_watch(scope, watch, directory, executable, outside)
            finally:
                watch.close()

            (directory / 'renamed.bin').unlink()
            summary, _ = traced_build(ycxx, scope, env, 'warm restore of a missing asset')
            assert summary['actions_executed'] == 0, summary
            previous = check_assets(directory, expected, absent=old_names)
            check_foreign(directory, outside)
            initial_daemon = daemon_identity(ycxx, scope, env)
            assert initial_daemon is not None, 'initial daemon exited'
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')

        env = env_for(restarted_runtime)
        try:
            replace_source(scope, 'build.ycxx', manifest('foreign.bin'))
            summary, _ = traced_build(ycxx, scope, env, 'reject claim on foreign file', succeeds=False)
            assert any('foreign.bin' in diagnostic for diagnostic in summary['diagnostics']), summary
            daemon = daemon_identity(ycxx, scope, env)
            assert daemon is not None and daemon != initial_daemon, (initial_daemon, daemon)
            assert check_assets(directory, expected, absent=old_names) == previous
            check_foreign(directory, outside)

            replace_source(scope, 'build.ycxx', manifest(None))
            traced_build(ycxx, scope, env, 'publish an empty runtime asset plan')
            check_assets(directory, {}, absent=(*old_names, 'renamed.bin'))
            check_foreign(directory, outside)
            check_program(executable, 4)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
