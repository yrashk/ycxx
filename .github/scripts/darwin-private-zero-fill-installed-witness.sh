#!/usr/bin/env bash
set -euo pipefail

python3 -B - "$@" <<'PY'
from hashlib import sha256
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile

candidate = Path(sys.argv[1]).resolve(strict=True)
receipt = Path(sys.argv[2]).resolve()
receipt.mkdir(parents=True, exist_ok=True)
journal = receipt / 'commands.jsonl'
journal.write_text('', encoding='utf-8')
host = platform.system()


def run(label, argv, *, cwd=None, environment=None, expected=0):
    argv = [str(value) for value in argv]
    result = subprocess.run(argv, cwd=cwd, env=environment, capture_output=True, timeout=180)
    stdout, stderr = f'{label}.stdout', f'{label}.stderr'
    (receipt / stdout).write_bytes(result.stdout)
    (receipt / stderr).write_bytes(result.stderr)
    with journal.open('a', encoding='utf-8') as out:
        out.write(json.dumps({'argv': argv, 'cwd': str(cwd or Path.cwd()), 'status': result.returncode,
                              'expected': expected, 'stdout': stdout, 'stderr': stderr}) + '\n')
    print(f'{shlex.join(argv)} [exit {result.returncode}]', flush=True)
    if expected is not None and result.returncode != expected:
        print(result.stdout.decode(errors='replace')[-12000:])
        print(result.stderr.decode(errors='replace')[-12000:], file=sys.stderr)
        raise SystemExit(f'{label} exited {result.returncode}, expected {expected}')
    return result


def trace(result):
    return [json.loads(line) for line in result.stderr.splitlines() if line.strip()]


with tempfile.TemporaryDirectory(prefix='yz393-', dir='/tmp') as directory:
    root = Path(directory).resolve()
    prefix = root / 'installed'
    run('install-sdk', ['cmake', '--install', candidate.parent.parent, '--prefix', prefix])
    driver = prefix / 'bin/ycxx'
    driver.parent.mkdir(exist_ok=True)
    shutil.copy2(candidate, driver)
    assert sha256(driver.read_bytes()).digest() == sha256(candidate.read_bytes()).digest()
    sdk = prefix / 'share/ycxx/resources/rules'
    installed_source = sdk / 'standard/rules.cc'
    installed_manifest = sdk / 'standard/build.ycxx'
    source_digests = {}
    for path in (installed_source, installed_manifest, sdk / 'rule_api.h'):
        relative = path.relative_to(sdk)
        checkout = Path.cwd() / 'resources/rules' / relative
        contents = path.read_bytes()
        assert checkout.read_bytes() == contents, relative
        source_digests[str(relative)] = sha256(contents).hexdigest()
    (receipt / 'source-digests.json').write_text(json.dumps(source_digests, indent=2) + '\n')

    project = root / 'project'
    (project / '.ycxx').mkdir(parents=True)
    (project / 'build.ycxx').write_text('cpp.executable "app" {\n    sources = ["main.cc"]\n}\n')
    (project / 'main.cc').write_text('int main() { return 17; }\n')
    tool_path = root / 'sdk-tools'
    tool_path.mkdir()
    if host == 'Darwin':
        (tool_path / 'xcrun').symlink_to('/usr/bin/xcrun')
    env = {key: value for key, value in os.environ.items() if key not in
           {'YCXX_DAEMON', 'YCXX_NO_DAEMON', 'YCXX_ROOT', 'YCXX_TRACE_ENGINE'}}
    env.update(PATH=str(tool_path), LC_ALL='C', YCXX_RUNTIME_DIR=str(root / 'runtime'), YCXX_THREADS='2')
    (root / 'runtime').mkdir()

    replay = root / 'replay'
    (replay / 'inputs/sdk').mkdir(parents=True)
    (replay / 'outputs').mkdir()
    shutil.copyfile(installed_source, replay / 'inputs/rules.cc')
    shutil.copyfile(sdk / 'rule_api.h', replay / 'inputs/sdk/ycxx_rule_api.h')
    replay_result = run('replay-compile',
                        [driver, 'cxx', '--no-daemon', '--trace-engine=json', '-nostdlib', '-nostdinc++',
                         '-ffreestanding', '-fPIC', '-shared', '-Iinputs/sdk', 'inputs/rules.cc',
                         '-o', 'outputs/rules.so'], cwd=replay, environment=env, expected=None)
    try:
        cold = run('cold-build', [driver, 'build', 'app', '--trace-engine=json'],
                   cwd=project, environment=env, expected=None)
        if cold.returncode != 0:
            print(cold.stderr.decode(errors='replace')[-12000:], file=sys.stderr)
            print(replay_result.stderr.decode(errors='replace')[-12000:], file=sys.stderr)
            raise SystemExit('installed standard provider build failed')
        events = trace(cold)
        packages = [(event['key'], event['outcome']) for event in events if
                    event.get('kind') == 'RulePackage' and event.get('outcome') in ('executed', 'failed')]
        assert packages == [(f'standard/rules via {driver}', 'executed')], packages
        assert any(event.get('kind') == 'RuleWorker' and event.get('key') == 'app' and
                   event.get('outcome') == 'executed' for event in events), events[-8:]
        program = project / '.ycxx/out/app'
        run('cold-app', [program], cwd=project, environment=env, expected=17)
        cold_binary = program.read_bytes()
        shutil.copyfile(program, receipt / 'app')
        warm = run('warm-build', [driver, 'build', 'app', '--trace-engine=json'], cwd=project, environment=env)
        assert trace(warm)[-1]['actions_executed'] == 0
        assert program.read_bytes() == cold_binary
        run('show', [driver, 'show', 'app', '--format=json'], cwd=project, environment=env)

        if replay_result.returncode != 0:
            print(replay_result.stderr.decode(errors='replace')[-12000:], file=sys.stderr)
            raise SystemExit('installed standard provider replay failed')
        provider = replay / 'outputs/rules.so'
        shutil.copyfile(provider, receipt / 'rules.so')
        if host == 'Darwin':
            assert platform.machine() == 'arm64'
            sys.path.insert(0, str(Path.cwd() / 'tests/unit/pipeline'))
            from darwin_shared_output import check_image
            metadata = check_image(provider.read_bytes(), 'rules.so', {b'_ycxx_rule_plan_v1'},
                                   require_unwind=True, expected_bss=(1024 * 1024, 0))
            (receipt / 'replay-layout.json').write_text(json.dumps(metadata, indent=2) + '\n')
            run('replay-signature', ['/usr/bin/codesign', '--verify', '--strict', '--verbose=4', provider])
            layout = run('replay-layout', ['/usr/bin/otool', '-l', provider]).stdout.decode()
            assert 'sectname __bss' in layout and 'segname __DATA' in layout, layout

        properties = '{\n    sources = ["main.cc"]\n}'
        for label, definition in (
            ('small', properties), ('near-capacity', properties[:-1] + ' ' * (1024 * 1024 - 144) + '}'),
        ):
            request = replay / f'{label}-request.json'
            request.write_text(json.dumps({'version': 1, 'target': 'app', 'rule': 'executable',
                                           'definition': definition, 'attributes': [], 'reads': [],
                                           'capabilities': None, 'witness': None, 'facts': None}) + '\n')
            shutil.copyfile(request, receipt / request.name)
            plan = replay / f'{label}-plan.ycxx'
            run(f'{label}-worker', [driver, '--internal-rule-worker', provider, request, plan],
                cwd=replay, environment=env)
            assert plan.read_text() == 'native.executable "app" ' + definition, plan.stat().st_size
            shutil.copyfile(plan, receipt / plan.name)
        print('Bundled plan near-capacity bytes:', (receipt / 'near-capacity-plan.ycxx').stat().st_size)
        for path in (receipt / 'app', receipt / 'rules.so'):
            print(sha256(path.read_bytes()).hexdigest(), path.name)
    finally:
        run('shutdown', [driver, 'ctl', 'shutdown'], cwd=project, environment=env, expected=None)
PY
