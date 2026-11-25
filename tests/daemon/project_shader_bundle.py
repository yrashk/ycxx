"""Build, inspect, and watch the shader runtime asset example (#1258)."""

import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

from project_build import action_keys, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_inspection import fact
from runtime_dir import runtime_root
from shader_compiler_fixture import spirv


EXAMPLE = Path(__file__).resolve().parents[2] / 'examples/project-build/shader-bundle'
FIXTURE = Path(__file__).resolve().with_name('shader_compiler_fixture.py')
FRAGMENT = 'triangle-fragment'
SHADERS = {
    FRAGMENT: ('fragment', 'shaders/triangle.frag'),
    'triangle-vertex': ('vertex', 'shaders/triangle.vert'),
}
BUNDLE = '.ycxx/assets/app/scene.bundle'


def compile_key(name):
    return f'scene/compile_{name}'


def prepare(scope):
    shutil.copytree(EXAMPLE, scope, ignore=shutil.ignore_patterns('.ycxx'))
    compiler = scope / 'glslc-fixture'
    python = str(Path(sys.executable).resolve())
    fixture = FIXTURE.read_text(encoding='utf-8')
    compiler.write_text(f'#!/bin/sh\nexec {shlex.quote(python)} -c {shlex.quote(fixture)} "$@"\n',
                        encoding='utf-8')
    compiler.chmod(0o700)

    manifest = scope / 'build.ycxx'
    definition = manifest.read_text(encoding='utf-8')
    for default, replacement in (('/usr/bin/glslc', str(compiler)), ('/usr/bin/python3', python)):
        assert definition.count(json.dumps(default)) == 1, (default, definition)
        definition = definition.replace(json.dumps(default), json.dumps(replacement))
    manifest.write_text(definition, encoding='utf-8')


def check_bundle(scope):
    expected = ''.join(
        f'{name} {spirv(stage, (scope / source).read_bytes()).hex()}\n'
        for name, (stage, source) in sorted(SHADERS.items())
    ).encode('ascii')
    bundle = (scope / BUNDLE).read_bytes()
    assert bundle == expected, (bundle, expected)
    return bundle


def check_application(scope, *names):
    for name in names:
        result = subprocess.run([sys.executable, scope / 'run.py', name], cwd=scope,
                                capture_output=True, timeout=30)
        assert result.returncode == 0, (name, result.returncode, result.stdout, result.stderr)


def check_inspection(ycxx, scope, env):
    result = run_client(ycxx, scope, env, 'show', 'app', '--format=json')
    assert result.returncode == 0, result.stderr
    plan = json.loads(result.stdout)
    assert plan['schema'] == 1 and plan['target'] == 'app', plan
    for name in SHADERS:
        binding = f'shader_{name}'
        item = fact(plan, 'input', 'scene', f'artifacts/{binding}/{name}.spv')
        assert item['name'] == binding, item
        assert item['origin'] == f'artifact("compile_{name}", "spirv")', item
    asset = fact(plan, 'runtime-asset', 'app', BUNDLE)
    assert asset['name'] == 'scene.bundle' and asset['origin'] == 'artifact("scene", "bundle")', asset


def check_external_actions(events, *executed, failed=()):
    actual = action_keys(events, 'ExternalAction'), action_keys(events, 'ExternalAction', 'failed')
    assert actual == (sorted(executed), sorted(failed)), actual


def check_shader_change(summary, events):
    assert summary['actions_executed'] == 2, summary
    check_external_actions(events, 'scene', compile_key(FRAGMENT))


def publication(path):
    status = path.stat()
    return path.read_bytes(), status.st_ino, status.st_mtime_ns, status.st_ctime_ns


def check_native_reused(events, executable, installed):
    ran = {'executed', 'cutoff', 'recomputed', 'failed'}
    links = [(event['key'], event['outcome']) for event in events if event.get('kind') == 'Link']
    assert not any(outcome in ran for _, outcome in links), links
    assert not ran & tu_outcomes(events, 'main.cc'), 'unchanged main.cc ran'
    assert publication(executable) == installed, 'shader build replaced the native executable'


def edit_plugin(scope):
    relative = 'rules/shaders.cc'
    source = (scope / relative).read_text(encoding='utf-8')
    anchor = '    args = [input("packer"), output("bundle")'
    # Change the worker's emitted text while keeping the same stages and recipes.
    assert source.count(anchor) == 1, source
    replace_source(scope, relative, source.replace(anchor, '\n' + anchor))


def check_watch(ycxx, scope, env, executable, installed, changed, recovery):
    _, fragment_source = SHADERS[FRAGMENT]
    fragment_path = str(scope / fragment_source)
    bundle = scope / BUNDLE
    installed_bundle = publication(bundle)
    watch = Watch(ycxx, scope, env)
    try:
        summary, events, watched = watch.build('watch unchanged shaders')
        assert summary['actions_executed'] == 0, summary
        assert {str(scope / source) for _, source in SHADERS.values()} <= set(watched), watched
        check_native_reused(events, executable, installed)

        replace_source(scope, fragment_source, changed + '\n#error invalid fragment shader\n')
        summary, events, watched = watch.build('watch rejects bad fragment shader', succeeds=False)
        assert summary['actions_executed'] == 1, summary
        check_external_actions(events, failed=(compile_key(FRAGMENT),))
        assert any('shader rejected by fixture' in diagnostic for diagnostic in summary['diagnostics']), summary
        assert fragment_path in watched, watched
        assert publication(bundle) == installed_bundle, 'failed shader build replaced the previous bundle'
        check_native_reused(events, executable, installed)
        check_application(scope, FRAGMENT)

        replace_source(scope, fragment_source, recovery)
        summary, events, _ = watch.build('watch recovers with corrected fragment shader')
        check_shader_change(summary, events)
        recovered = check_bundle(scope)
        assert recovered != installed_bundle[0], 'recovery kept stale shader bytes'
        check_native_reused(events, executable, installed)
        check_application(scope, FRAGMENT)
        watch.idle('shader asset publication after recovery')

        (scope / fragment_source).unlink()
        summary, events, watched = watch.build('watch detects missing fragment source', succeeds=False)
        assert summary['actions_executed'] == 0, summary
        assert fragment_path in watched and bundle.read_bytes() == recovered, watched
        check_native_reused(events, executable, installed)
        watch.idle('missing shader source waits for restoration')
        replace_source(scope, fragment_source, recovery)
        summary, events, _ = watch.build('watch recovers when the source reappears')
        assert summary['actions_executed'] == 0, summary
        assert check_bundle(scope) == recovered
        check_native_reused(events, executable, installed)
        return recovered
    finally:
        watch.close()


def check_builds(ycxx, scope, env):
    executable = scope / '.ycxx/out/app'
    bundle = scope / BUNDLE
    _, events = traced_build(ycxx, scope, env, 'cold shader bundle')
    check_external_actions(events, 'scene', 'release_note', *(compile_key(name) for name in SHADERS))
    assert 'executed' in tu_outcomes(events, 'main.cc') and action_keys(events, 'Link'), 'cold native build did not run'
    installed = publication(executable)
    check_bundle(scope)
    first_bundle = publication(bundle)
    check_application(scope, *SHADERS)
    check_inspection(ycxx, scope, env)

    summary, events = traced_build(ycxx, scope, env, 'unchanged shader bundle')
    assert summary['actions_executed'] == 0, summary
    check_external_actions(events)
    assert publication(bundle) == first_bundle, 'unchanged build replaced the bundle'
    check_native_reused(events, executable, installed)

    _, fragment_source = SHADERS[FRAGMENT]
    source = (scope / fragment_source).read_text(encoding='utf-8')
    original = 'vec4(1.0, 0.2, 0.3, 1.0)'
    assert source.count(original) == 1, source
    changed = source.replace(original, 'vec4(0.25, 0.2, 0.3, 1.0)')
    recovery = source.replace(original, 'vec4(0.75, 0.2, 0.3, 1.0)')
    replace_source(scope, fragment_source, changed)
    summary, events = traced_build(ycxx, scope, env, 'change only the fragment shader')
    check_shader_change(summary, events)
    assert check_bundle(scope) != first_bundle[0], 'shader edit kept stale bundle bytes'
    changed_bundle = publication(bundle)
    check_native_reused(events, executable, installed)

    edit_plugin(scope)
    _, events = traced_build(ycxx, scope, env, 'equivalent shader rule expansion')
    assert 'scene' in action_keys(events, 'RuleWorker'), 'edited plugin did not emit a new expansion'
    check_external_actions(events)
    assert publication(bundle) == changed_bundle, 'equivalent rule edit replaced the bundle'
    check_native_reused(events, executable, installed)

    recovered = check_watch(ycxx, scope, env, executable, installed, changed, recovery)
    bundle.unlink()
    summary, events = traced_build(ycxx, scope, env, 'restore the removed bundle from receipts')
    assert summary['actions_executed'] == 0, summary
    check_external_actions(events)
    assert bundle.read_bytes() == recovered, 'restored bundle differs from the last successful output'
    check_native_reused(events, executable, installed)


def main():
    ycxx = Path(sys.argv[1]).resolve()
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-shader-bundle-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        scope = Path(workspace).resolve() / 'fixture'
        prepare(scope)
        env = env_for(runtime)
        try:
            check_builds(ycxx, scope, env)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
