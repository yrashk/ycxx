"""Exercise receipt-backed planning inputs through the resident daemon (#1335)."""

import json
from pathlib import Path
import re
import shlex
import sys
import tempfile

from project_build import action_keys, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_inspection import check_location, fact
from project_shader_bundle import (
    BUNDLE, SHADERS, check_application, check_external_actions, compile_key, prepare, publication,
)
from runtime_dir import runtime_root
from shader_compiler_fixture import spirv


INVENTORY = 'shaders/inventory.txt'
GENERATOR = 'shader_inventory'
ARTIFACT = 'artifact("shader_inventory", "list")'
PLAN_PATH = '.ycxx/virtual/shader_inventory/inventory.txt'
ASSET_PATH = '.ycxx/assets/app/inventory.txt'
ASSET_LIST = 'artifact("release_note", "note")]'
ACCENT = 'triangle-accent'
ACCENT_SOURCE = 'shaders/accent.frag'
ACCENT_ROW = f'{ACCENT} fragment {ACCENT_SOURCE}\n'
NATIVE = '.ycxx/out/app'
BACK_EDGE = r'''
generate "cycle_inventory" {
    tool = "/bin/sh"
    args = ["-c", "cat \"$1\" > \"$2\"", "cycle", input("bundle"), output("list")]
    inputs = { bundle = artifact("scene", "bundle") }
    outputs = { list = "cycle-inventory.txt" }
}
'''


def prepare_inventory(scope, journal):
    prepare(scope)
    manifest = scope / 'build.ycxx'
    definition = manifest.read_text(encoding='utf-8')
    assert definition.count(ASSET_LIST) == 1, definition
    published = definition.replace(ASSET_LIST, f'{ASSET_LIST[:-1]}, {ARTIFACT}]')
    manifest.write_text(published, encoding='utf-8')

    # Record each actual generation outside the workspace's observed paths.
    script = scope / 'rules/inventory.sh'
    source = script.read_text(encoding='utf-8')
    first, rest = source.split('\n', 1)
    script.write_text(f"{first}\nprintf '%s\\n' generated >> {shlex.quote(str(journal))}\n{rest}",
                      encoding='utf-8')
    return definition


def journal_contents(journal):
    return journal.read_text(encoding='utf-8') if journal.exists() else ''


def inspect(ycxx, scope, env, journal, *, unresolved=None, published=False):
    before = journal_contents(journal)
    result = run_client(ycxx, scope, env, 'show', 'app', '--format=json')
    assert result.returncode == 0, result.stderr
    assert journal_contents(journal) == before, 'show executed the inventory generator'
    plan = json.loads(result.stdout)
    assert plan['schema'] == 1 and plan['target'] == 'app', plan
    if unresolved is not None:
        assert any(row['kind'] == 'unresolved' and row['target'] == unresolved
                   for row in plan['facts']), plan
        assert not any(row['kind'] == 'plan-input' and row['target'] == 'scene'
                       for row in plan['facts']), plan
        return None

    item = fact(plan, 'plan-input', 'scene', PLAN_PATH)
    assert item['name'] == 'inventory', item
    check_location(item, plan['definition'], ARTIFACT)
    assert re.fullmatch(re.escape(f'{ARTIFACT} · receipt ') + r'[0-9a-f]{32}', item['origin']), item
    output = fact(plan, 'output', GENERATOR, PLAN_PATH)
    assert output['name'] == 'list', output
    if published:
        asset = fact(plan, 'runtime-asset', 'app', ASSET_PATH)
        assert asset['origin'] == ARTIFACT, asset
    return item['origin']


def check_bundle(scope, *, accent=False):
    shaders = {**SHADERS, ACCENT: ('fragment', ACCENT_SOURCE)} if accent else SHADERS
    expected = ''.join(f'{name} {spirv(stage, (scope / path).read_bytes()).hex()}\n'
                       for name, (stage, path) in sorted(shaders.items()))
    actual = (scope / BUNDLE).read_text(encoding='ascii')
    assert actual == expected, (actual, expected)
    check_application(scope, *shaders)
    return actual


def native_publication(scope):
    status = (scope / NATIVE).stat()
    return status.st_ino, status.st_mtime_ns, status.st_ctime_ns


def check_native_reuse(scope, events, installed):
    ran = {'executed', 'cutoff', 'recomputed', 'failed'}
    links = [(event['key'], event['outcome']) for event in events if event.get('kind') == 'Link']
    assert not any(outcome in ran for _, outcome in links), links
    assert not ran & tu_outcomes(events, 'main.cc'), 'unchanged main.cc ran'
    assert native_publication(scope) == installed, 'inventory edit replaced the native executable'


def check_watch(ycxx, scope, env, journal, installed, stable, expanded):
    source = str(scope / INVENTORY)
    assets = (scope / BUNDLE, scope / '.ycxx/assets/app/release-note.txt')
    watch = Watch(ycxx, scope, env)
    try:
        _, events, watched = watch.build('watch inventory used only for planning')
        assert source in watched and str(scope / 'rules/inventory.sh') in watched, watched
        check_external_actions(events)
        check_native_reuse(scope, events, installed)
        assert not (scope / ASSET_PATH).exists(), 'inventory still has an ordinary consumer'
        installed_assets = [publication(path) for path in assets]

        replace_source(scope, INVENTORY, stable + 'malformed inventory row with extra fields\n')
        summary, events, watched = watch.build('generated inventory is invalid', succeeds=False)
        check_external_actions(events, GENERATOR)
        assert any('inventory row' in diagnostic for diagnostic in summary['diagnostics']), summary
        assert source in watched, watched
        assert [publication(path) for path in assets] == installed_assets, 'invalid inventory changed assets'
        check_native_reuse(scope, events, installed)
        check_application(scope, *SHADERS)
        inspect(ycxx, scope, env, journal, unresolved='scene')

        (scope / INVENTORY).unlink()
        _, events, watched = watch.build('inventory generator input is missing', succeeds=False)
        check_external_actions(events)
        assert source in watched, watched
        assert [publication(path) for path in assets] == installed_assets, 'missing inventory changed assets'
        check_native_reuse(scope, events, installed)

        replace_source(scope, INVENTORY, expanded)
        summary, events, _ = watch.build('watch restores a valid inventory')
        assert summary['actions_executed'] == 0, summary
        check_external_actions(events)
        check_bundle(scope, accent=True)
        check_native_reuse(scope, events, installed)
        inspect(ycxx, scope, env, journal)
        watch.idle('watch after restored inventory publication')
    finally:
        watch.close()


def check_builds(ycxx, scope, env, journal):
    definition = prepare_inventory(scope, journal)
    original = (scope / INVENTORY).read_text(encoding='utf-8')
    inspect(ycxx, scope, env, journal, unresolved='app')
    assert not (scope / '.ycxx').exists(), 'cold show created build state'

    _, events = traced_build(ycxx, scope, env, 'cold generated inventory')
    check_external_actions(events, GENERATOR, 'release_note', 'scene',
                           *(compile_key(name) for name in SHADERS))
    assert journal_contents(journal).splitlines() == ['generated'], 'the two consumers generated twice'
    assert 'scene' in action_keys(events, 'RuleWorker'), events
    assert 'executed' in tu_outcomes(events, 'main.cc') and action_keys(events, 'Link'), events
    installed = native_publication(scope)
    installed_bundle = publication(scope / BUNDLE)
    installed_inventory = publication(scope / ASSET_PATH)
    assert installed_inventory[0] == original.encode('utf-8'), installed_inventory
    initial_bundle = check_bundle(scope)
    initial_receipt = inspect(ycxx, scope, env, journal, published=True)

    stable = original + '# Inventory comment filtered by the generator\n'
    replace_source(scope, INVENTORY, stable)
    inspect(ycxx, scope, env, journal, unresolved='scene')
    summary, events = traced_build(ycxx, scope, env, 'generated list has equal bytes')
    assert summary['actions_executed'] == 1, summary
    check_external_actions(events, GENERATOR)
    assert not action_keys(events, 'RuleWorker'), events
    assert publication(scope / BUNDLE) == installed_bundle, 'equal inventory changed the bundle'
    assert publication(scope / ASSET_PATH) == installed_inventory, 'equal inventory changed the published list'
    check_native_reuse(scope, events, installed)
    stable_receipt = inspect(ycxx, scope, env, journal, published=True)
    assert stable_receipt != initial_receipt, 'show kept stale receipt provenance for equal inventory bytes'

    expanded = stable + ACCENT_ROW
    replace_source(scope, INVENTORY, expanded)
    inspect(ycxx, scope, env, journal, unresolved='scene')
    _, events = traced_build(ycxx, scope, env, 'add one shader to the inventory')
    check_external_actions(events, GENERATOR, compile_key(ACCENT), 'scene')
    assert action_keys(events, 'RuleWorker') == ['scene'], events
    assert (scope / ASSET_PATH).read_text(encoding='utf-8') == original + ACCENT_ROW
    assert check_bundle(scope, accent=True) != initial_bundle
    check_native_reuse(scope, events, installed)
    assert inspect(ycxx, scope, env, journal, published=True) != stable_receipt

    replace_source(scope, INVENTORY, stable)
    summary, events = traced_build(ycxx, scope, env, 'remove the additional shader')
    assert summary['actions_executed'] == 0, summary
    check_external_actions(events)
    assert check_bundle(scope) == initial_bundle
    assert (scope / ASSET_PATH).read_bytes() == installed_inventory[0]
    check_native_reuse(scope, events, installed)
    assert inspect(ycxx, scope, env, journal, published=True) == stable_receipt

    # The watch below must pick up the producer through the planning edge alone.
    replace_source(scope, 'build.ycxx', definition)
    check_watch(ycxx, scope, env, journal, installed, stable, expanded)


def check_cycles(ycxx, workspace, env):
    for name, reference, extra in (
        ('self', 'artifact("scene", "bundle")', ''),
        ('mixed', 'artifact("cycle_inventory", "list")', BACK_EDGE),
    ):
        scope = workspace / name
        prepare(scope)
        manifest = scope / 'build.ycxx'
        definition = manifest.read_text(encoding='utf-8')
        assert definition.count(ARTIFACT) == 1, definition
        manifest.write_text(definition.replace(ARTIFACT, reference) + extra, encoding='utf-8')
        try:
            summary, events = traced_build(ycxx, scope, env, f'{name} planning cycle', succeeds=False)
            diagnostics = summary['diagnostics']
            assert any('planning input dependency cycle' in diagnostic and 'scene' in diagnostic
                       for diagnostic in diagnostics), diagnostics
            check_external_actions(events)
            assert 'scene' not in action_keys(events, 'RuleWorker'), events
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


def main():
    ycxx = Path(sys.argv[1]).resolve()
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-generated-inventory-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        workspace = Path(workspace).resolve()
        scope = workspace / 'fixture'
        env = env_for(runtime)
        try:
            check_builds(ycxx, scope, env, workspace / 'inventory-runs')
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')
        check_cycles(ycxx, workspace, env)


if __name__ == '__main__':
    main()
