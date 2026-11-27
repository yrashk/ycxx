"""Measure the shared output and worker load of a ycxx-built rule package (#1678)."""

import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from project_build import action_keys, daemon_identity, env_for
from project_inspection import scope_state
from project_targets import DARWIN, LINUX, executable
from project_toolchain_probe import probe_actions
from runtime_dir import runtime_root


REPO = Path(__file__).resolve().parents[2]
EXAMPLE = REPO / 'examples/project-build/native-leaf-rule'
PACKAGE = 'rules/package'
PROBES = {'object.host-shared': 'object', 'link.host-shared': 'shared library',
          'link.host-shared.load': 'shared library'}


def fact(plan, kind, target, name):
    found = [row for row in plan['facts']
             if (row['kind'], row['target'], row['name']) == (kind, target, name)]
    assert len(found) == 1, (kind, target, name, found)
    return found[0]


def probe_keys(events, name, outcomes):
    return {row['key'] for row in events if row.get('kind') == 'ToolchainProbeResult'
            and row['outcome'] in outcomes and row['key'].startswith(f'{name} [')}


def image_metadata(data, triple, *, shared=False):
    kind_name = 'shared library' if shared else 'executable'
    if triple == LINUX:
        assert data[:6] == b'\x7fELF\x02\x01', data[:20]
        kind, machine = struct.unpack_from('<2H', data, 16)
        assert (kind, machine) == (3 if shared else 2, 62), (kind, machine)  # ET_DYN/ET_EXEC, EM_X86_64
        return {'format': 'ELF', 'architecture': 'x86_64', 'kind': kind_name}
    magic, cpu, _, kind = struct.unpack_from('<4I', data)
    assert (magic, cpu, kind) == (0xfeedfacf, 0x100000c, 6 if shared else 2), (magic, cpu, kind)
    return {'format': 'Mach-O', 'architecture': 'aarch64', 'kind': kind_name}


def check_plan(plan, ycxx, triple, artifact):
    selected = fact(plan, 'toolchain-compiler', PACKAGE, 'driver')
    assert (selected['value'], selected['origin']) == (str(ycxx), 'Selected native compiler'), selected
    for name, kind in PROBES.items():
        probe = fact(plan, 'toolchain-capability', PACKAGE, name)
        assert probe['value'] == 'supported', probe
        assert triple in probe['origin'] and f'{artifact} {kind}' in probe['origin'], probe
    freestanding = fact(plan, 'toolchain-capability', PACKAGE, 'link.freestanding')
    assert freestanding['value'] == 'supported', freestanding
    assert triple in freestanding['origin'] and f'{artifact} executable' in freestanding['origin'], freestanding
    loaded = fact(plan, 'toolchain-capability', PACKAGE, 'link.host-shared.load')
    assert 'loaded library and called ycxx_rule_plan_v1 (exit 0)' in loaded['origin'], loaded

    output = fact(plan, 'output', 'score', 'header')
    assert Path(output['value']).name == 'leaf_score.hh', output
    includes = [row for row in plan['facts'] if (row['kind'], row['target'], row['origin']) ==
                ('include', 'app', 'artifact("score", "header")')]
    assert len(includes) == 1, includes
    definitions = [row['value'] for row in plan['facts'] if (row['kind'], row['target']) == ('argv', 'score')
                   and 'inline constexpr int leaf_score = 17;' in row['value']]
    assert len(definitions) == 1, definitions


def replay_package(ycxx, scope, env, triple, run, keep):
    # Replay the example's native recipe in its staged layout. These receipts
    # are separate from the ephemeral actions observed in the build trace.
    manifest = (scope / 'rules/build.ycxx').read_text(encoding='utf-8')

    def recipe(field):
        value, = re.findall(rf'^\s*{field}\s*=\s*(\[[^\]]*\])', manifest, re.M)
        return value

    source, = re.findall(r'\bsource\s*=\s*"([^"]+)"', manifest)
    plugin, = re.findall(r'\bplugin\s*=\s*"([^"]+)"', manifest)
    replay = scope.parent / 'replay'
    (replay / 'inputs/sdk').mkdir(parents=True)
    (replay / 'outputs').mkdir()
    shutil.copyfile(scope / 'rules' / source, replay / 'inputs' / source)
    shutil.copyfile(REPO / 'resources/rules/rule_api.h', replay / 'inputs/sdk/ycxx_rule_api.h')
    args = recipe('args').replace('input("source")', json.dumps(f'inputs/{source}'))
    args = args.replace('output("plugin")', json.dumps(f'outputs/{plugin}'))
    compiler_env = {}
    if 'PATH' in env:
        compiler_env['PATH'] = os.pathsep.join(str(scope / path) for path in env['PATH'].split(os.pathsep))
    run('replay-compile', [ycxx, 'cxx', '--no-daemon', '-nostdlib',
                           *json.loads(recipe('native_args')), *json.loads(args)],
        cwd=replay, environment=compiler_env)
    library = (replay / 'outputs' / plugin).read_bytes()
    keep('replay-rules.so', library)
    metadata = image_metadata(library, triple, shared=True)
    keep('replay-rules.json', (json.dumps(metadata, indent=2) + '\n').encode('utf-8'))
    # PARITY: by design: only native macOS verifies the arm64 Mach-O code signature.
    if triple == DARWIN:
        run('replay-signature', ['/usr/bin/codesign', '--verify', '--strict', '--verbose=2', f'outputs/{plugin}'],
            cwd=replay)

    (replay / 'inputs/package').mkdir()
    (replay / 'inputs/package/rules.so').write_bytes(library)
    request = (json.dumps({'version': 1, 'target': 'app', 'rule': 'executable', 'definition': '{}',
                           'attributes': [], 'reads': [], 'capabilities': None, 'witness': None, 'facts': None})
               + '\n').encode('utf-8')
    (replay / 'inputs/request.json').write_bytes(request)
    keep('replay-request.json', request)
    run('replay-worker', [ycxx, '--internal-rule-worker', 'inputs/package/rules.so',
                          'inputs/request.json', 'outputs/plan.ycxx'], cwd=replay, environment={})
    plan = (replay / 'outputs/plan.ycxx').read_bytes()
    keep('replay-plan.ycxx', plan)
    assert plan == b'''native.executable "app" {
    sources = ["main.cc"]
    generated_headers = [artifact("score", "header")]
}
''', plan


def exercise(ycxx, scope, env, receipts):
    triple, artifact = {'linux': (LINUX, 'x86_64 ELF'), 'darwin': (DARWIN, 'aarch64 Mach-O')}[sys.platform]
    app = scope / executable(None)

    def keep(name, data):
        if receipts is not None:
            (receipts / name).write_bytes(data)

    def run(label, command, expected=0, *, cwd=scope, environment=env):
        result = subprocess.run(command, cwd=cwd, env=environment, capture_output=True, timeout=120)
        if receipts is not None:
            stdout, stderr = label + '.stdout', label + '.stderr'
            keep(stdout, result.stdout)
            keep(stderr, result.stderr)
            outcome = {'argv': [str(arg) for arg in command], 'cwd': str(cwd), 'status': result.returncode,
                       'expected_status': expected, 'stdout': stdout, 'stderr': stderr}
            with (receipts / 'commands.jsonl').open('a', encoding='utf-8') as journal:
                journal.write(json.dumps(outcome) + '\n')
        if expected is not None:
            assert result.returncode == expected, (label, command, result.returncode, result.stderr[-6000:])
        return result

    def show(label):
        before = scope_state(scope)
        plan = json.loads(run(label, [ycxx, 'show', 'app', '--format=json']).stdout)
        assert plan['schema'] == 1 and plan['target'] == 'app', plan
        assert scope_state(scope) == before, 'show wrote project state'
        return plan

    def build(label):
        reply = run(label, [ycxx, 'build', 'app', '--probe-toolchain', '--trace-engine=json'])
        records = [json.loads(line) for line in reply.stderr.splitlines() if line.strip()]
        summary = records[-1]
        assert summary['status'] == 0 and (scope / summary['output']).resolve() == app, summary
        return summary, records

    try:
        before = show('cold-show')
        assert not any(row['kind'].startswith('toolchain-') for row in before['facts']), before
        assert not (scope / '.ycxx').exists(), 'cold show created project state'

        summary, cold = build('cold-build')
        assert summary['actions_executed'] > 0, summary
        assert action_keys(cold, 'RulePackage') == [f'{PACKAGE} via {ycxx}'], cold
        assert not action_keys(cold, 'RulePackage', 'failed'), cold
        assert action_keys(cold, 'RuleWorker') == ['app', 'score'], cold
        assert action_keys(cold, 'ExternalAction') == ['score'], cold
        assert action_keys(cold, 'Link'), cold
        for name, event in (('object.host-shared', 'ToolchainProbeAction'),
                            ('link.host-shared', 'ToolchainProbeAction'),
                            ('link.host-shared.load', 'ToolchainProbeLoad')):
            assert f'{name} via {ycxx}' in action_keys(cold, event), probe_actions(cold)
            assert probe_keys(cold, name, {'executed'}), name
        initial_daemon = daemon_identity(ycxx, scope, env)
        assert initial_daemon is not None and initial_daemon[0] == str(scope), initial_daemon

        image = app.read_bytes()
        metadata = image_metadata(image, triple)
        keep('app', image)
        keep('app.json', (json.dumps(metadata, indent=2) + '\n').encode('utf-8'))
        installed = app.stat()
        run('run-app', [app], expected=17)
        plan = show('show')
        check_plan(plan, ycxx, triple, artifact)

        summary, warm = build('warm-build')
        assert summary['actions_executed'] == 0 and not probe_actions(warm), summary
        assert not any(action_keys(warm, kind) for kind in ('RulePackage', 'RuleWorker', 'ExternalAction', 'Link')), warm
        for name in PROBES:
            assert probe_keys(cold, name, {'executed'}) & probe_keys(warm, name, {'reused', 'verified', 'durable'}), name
        assert daemon_identity(ycxx, scope, env) == initial_daemon
        current = app.stat()
        assert (current.st_ino, current.st_mtime_ns) == (installed.st_ino, installed.st_mtime_ns), current
        assert app.read_bytes() == image
        assert show('warm-show') == plan, 'unchanged probe changed the published plan'
        run('run-warm-app', [app], expected=17)
        if receipts is not None:
            before_html = scope_state(scope)
            run('show-html', [ycxx, 'show', 'app', '--format=html'])
            assert scope_state(scope) == before_html, 'HTML inspection wrote project state'
            replay_package(ycxx, scope, env, triple, run, keep)
    finally:
        run('shutdown', [ycxx, 'ctl', 'shutdown'], expected=None)


def main():
    ycxx = Path(sys.argv[1]).resolve()
    receipt_path = os.environ.get('YCXX_NATIVE_RULE_PROBE_RECEIPTS')
    receipts = Path(receipt_path).resolve() if receipt_path else None
    if receipts is not None:
        receipts.mkdir(parents=True, exist_ok=True)
        (receipts / 'commands.jsonl').write_text('', encoding='utf-8')
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-native-rule-probe-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        scope = Path(workspace).resolve() / 'example'
        shutil.copytree(EXAMPLE, scope, ignore=shutil.ignore_patterns('.ycxx'))
        exercise(ycxx, scope, env_for(runtime), receipts)


if __name__ == '__main__':
    main()
