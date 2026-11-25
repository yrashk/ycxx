"""Cold and resident project builds agree across source and generator edits (#1255)."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from runtime_dir import runtime_root


GENERATOR = r'''
IFS= read -r value < "$1" || exit 5
if [ "$value" = rejected ]; then
    printf 'partial\n' > "$2"
    printf 'schema rejected\n' >&2
    exit 7
fi
printf 'int wire() { return %s; }\n' "$value" > "$2" || exit 5
printf 'inline int header_value() { return 5; }\n' > "$3"
'''


def run_client(ycxx, cwd, env, *args):
    return subprocess.run([ycxx, *args], cwd=cwd, env=env, capture_output=True,
                          text=True, encoding='utf-8', timeout=120)


def prepare(scope):
    (scope / '.ycxx').mkdir()
    (scope / 'include').mkdir()
    (scope / 'fallback').mkdir()
    (scope / 'fallback' / 'config.h').write_text('#define CONFIG 3\n')
    (scope / 'core.cc').write_text('int core() { return 2; }\n')
    (scope / 'main.cc').write_text('''\
#include <config.h>
#include "wire.h"
int core();
int wire();
int main() { return core() + wire() + header_value() + CONFIG; }
''')
    (scope / 'build.ycxx').write_text(f'''\
generate "wire" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(GENERATOR)}, "generator",
            input("input"), output("source"), output("header")]
    inputs = {{ input = "schema.txt" }}
    outputs = {{ source = "wire.cc" header = "wire.h" }}
}}
cpp.library "core" {{
    sources = ["core.cc"]
    public_include_dirs = ["include", "fallback"]
}}
cpp.executable "app" {{
    sources = ["main.cc", artifact("wire", "source")]
    generated_headers = [artifact("wire", "header")]
    deps = ["core"]
}}
''')
    (scope / 'schema.txt').write_text('7\n')


def traced_build(ycxx, scope, env, label, *, succeeds=True):
    p = run_client(ycxx, scope, env, 'build', 'app', '--trace-engine=json')
    assert (p.returncode == 0) == succeeds, (label, p.returncode, p.stderr[-5000:])
    try:
        records = [json.loads(line) for line in p.stderr.splitlines() if line.strip()]
    except ValueError as error:
        raise AssertionError((label, 'stderr is not JSON Lines', p.stderr[-5000:])) from error
    assert records and all(isinstance(record, dict) for record in records), (label, records)
    summary = records[-1]
    assert summary.get('status') == p.returncode, (label, summary)
    assert type(summary.get('actions_executed')) is int, (label, summary)
    assert isinstance(summary.get('diagnostics'), list), (label, summary)
    if succeeds:
        assert (scope / summary['output']).resolve() == scope / '.ycxx' / 'out' / 'app', (label, summary)
    return summary, records


def tu_outcomes(events, file):
    return {event['outcome'] for event in events
            if event.get('kind') == 'Tu' and Path(event['key']).name == file}


def action_keys(events, kind, outcome='executed'):
    return sorted(event['key'] for event in events
                  if event.get('kind') == kind and event.get('outcome') == outcome)


def check_cold_actions(summary, events):
    packages = action_keys(events, 'RulePackage')
    failed = action_keys(events, 'RulePackage', 'failed')
    assert len(packages) == 1 and len(failed) <= 1, (packages, failed)
    assert all(key.startswith('standard/rules via ') for key in packages + failed), (packages, failed)
    assert action_keys(events, 'RuleWorker') == ['app', 'core', 'wire'], events
    assert action_keys(events, 'ExternalAction') == ['wire'], events
    assert summary['actions_executed'] == 5 + len(failed), summary


def check_wire_action(summary, events, outcome='executed'):
    assert summary['actions_executed'] == 1, summary
    assert action_keys(events, 'ExternalAction', outcome) == ['wire'], events


def check_program(executable, expected):
    p = subprocess.run([executable], cwd=executable.parent, capture_output=True, timeout=30)
    assert p.returncode == expected, (executable, expected, p.returncode, p.stderr)


def check_edit_sequence(ycxx, scope, env):
    prepare(scope)
    executable = scope / '.ycxx' / 'out' / 'app'
    schema = scope / 'schema.txt'
    summary, events = traced_build(ycxx, scope, env, 'first build')
    check_cold_actions(summary, events)
    assert 'executed' in tu_outcomes(events, 'main.cc'), 'no native main.cc evaluation on first build'
    check_program(executable, 17)
    first = executable.read_bytes()
    installed_at = executable.stat().st_mtime_ns

    summary, events = traced_build(ycxx, scope, env, 'unchanged build')
    assert summary['actions_executed'] == 0, summary
    assert 'executed' not in tu_outcomes(events, 'main.cc'), 'unchanged native compile executed'
    assert executable.read_bytes() == first, 'unchanged build produced different executable bytes'
    assert executable.stat().st_mtime_ns == installed_at, 'unchanged build replaced the file'
    executable.unlink()
    summary, _ = traced_build(ycxx, scope, env, 'restore removed executable')
    assert summary['actions_executed'] == 0, summary
    assert executable.read_bytes() == first, 'restore changed executable bytes'

    # The workspace reserves its output directory. A symlink must not redirect publication.
    out = executable.parent
    saved = out.with_name('out-saved')
    out.rename(saved)
    with tempfile.TemporaryDirectory(prefix='ycxx-outside-') as outside:
        sentinel = Path(outside, 'app')
        sentinel.write_bytes(b'outside the workspace')
        out.symlink_to(outside, target_is_directory=True)
        try:
            summary, _ = traced_build(ycxx, scope, env, 'redirected output directory', succeeds=False)
            assert any('expected a build directory' in diagnostic for diagnostic in summary['diagnostics']), summary
            assert sentinel.read_bytes() == b'outside the workspace'
        finally:
            out.unlink()
            saved.rename(out)

    schema.write_text('11\n')
    summary, events = traced_build(ycxx, scope, env, 'generator edit')
    check_wire_action(summary, events)
    assert 'executed' in tu_outcomes(events, 'wire.cc'), 'changed generated source did not recompile'
    main = tu_outcomes(events, 'main.cc')
    assert main and main <= {'reused', 'verified', 'durable'}, ('unchanged header did not cut off main.cc', main)
    check_program(executable, 21)

    (scope / 'include' / 'config.h').write_text('#define CONFIG 13\n')
    _, events = traced_build(ycxx, scope, env, 'header shadow')
    assert 'executed' in tu_outcomes(events, 'main.cc'), 'new earlier include did not invalidate main.cc'
    check_program(executable, 31)
    incremental = executable.read_bytes()

    schema.write_text('rejected\n')
    summary, events = traced_build(ycxx, scope, env, 'failed generator', succeeds=False)
    check_wire_action(summary, events, 'failed')
    assert any('schema rejected' in diagnostic for diagnostic in summary['diagnostics']), summary
    assert executable.read_bytes() == incremental, 'failed generator replaced the last successful output'
    check_program(executable, 31)
    schema.write_text('11\n')
    return executable, incremental


def env_for(runtime):
    drop = {'YCXX_DAEMON', 'YCXX_NO_DAEMON', 'YCXX_NO_WATCH', 'YCXX_ROOT', 'YCXX_TRACE_ENGINE'}
    env = {key: value for key, value in os.environ.items() if key not in drop}
    env.update(YCXX_RUNTIME_DIR=runtime, YCXX_DAEMON_IDLE_EXIT='120',
               YCXX_DAEMON='require', YCXX_THREADS='2')
    return env


def main():
    ycxx = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='ycxx-project-') as workspace:
        scope = Path(workspace).resolve()
        with tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime:
            env = env_for(runtime)
            try:
                executable, incremental = check_edit_sequence(ycxx, scope, env)
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')

        # Keep the source paths and output name fixed while dropping resident and workspace state.
        shutil.rmtree(scope / '.ycxx')
        (scope / '.ycxx').mkdir()
        with tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime:
            env = env_for(runtime)
            try:
                summary, events = traced_build(ycxx, scope, env, 'cold build of final inputs')
                check_cold_actions(summary, events)
                assert executable.read_bytes() == incremental, 'incremental and cold executable bytes differ'
                check_program(executable, 31)
            finally:
                run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
