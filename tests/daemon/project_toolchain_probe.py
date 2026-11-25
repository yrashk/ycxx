"""Measure selected compiler profiles through the resident build and inspector (#1289)."""

import html
import json
from pathlib import Path
import shlex
import sys
import tempfile

from project_build import check_program, env_for, run_client, traced_build
from project_rule_package import generator_plan
from runtime_dir import runtime_root


STANDARD = 'standard/rules'
PINNED = 'cap/rules'
PROFILE_FLAG = '-DYCXX_MATRIX_TEST_HEADER=1'
REQUIRED = {'driver.default-target', 'dialect.cxx20', 'dialect.cxx23', 'dialect.cxx26',
            'header.cstddef', 'feature.concepts', 'object.default', 'object.arm64-darwin',
            'object.riscv64-linux', 'object.host-shared', 'link.hosted', 'link.hosted.run',
            'link.freestanding', 'link.host-shared', 'link.host-shared.load'}


def write_compiler(wrapper, compiler, log, *, reject_header):
    # The wrapper delegates ordinary compilation and successful probes to the host compiler.
    # It supplies one flag-dependent rejection and one clean exit without the requested artifact.
    script = f'''\
#!/bin/sh
printf '%s\\n' "$*" >> {shlex.quote(str(log))}
profile=0
header=0
for arg do
    case "$arg" in
        {PROFILE_FLAG}) profile=1 ;;
        --target=riscv64-unknown-linux-gnu) exit 0 ;;
        *.cc|*.cpp) if /usr/bin/grep -Fq '#include <cstddef>' "$arg"; then header=1; fi ;;
    esac
done
if [ {int(reject_header)} = 1 ] && [ "$profile" = 1 ] && [ "$header" = 1 ]; then
    printf '%s\\n' 'fixture: cstddef rejected by profile' >&2
    exit 1
fi
exec {shlex.quote(str(compiler))} "$@"
'''
    wrapper.write_text(script, encoding='utf-8')
    wrapper.chmod(0o700)


def write_package(scope, wrapper, *flags):
    shared = '-dynamiclib' if sys.platform == 'darwin' else '-shared'
    probe_flags = json.dumps(['-include', str(scope / 'profile.h'), *flags])
    (scope / 'cap/build.ycxx').write_text(f'''\
host_action "rules" {{
    tool = cxx({json.dumps(str(wrapper))})
    args = ["-std=c++20", "-fPIC", "{shared}", "-Iinputs/sdk",
            input("source"), "-o", output("plugin")]
    probe_flags = {probe_flags}
    environment = {{ PATH = "/usr/bin:/bin" }}
    inputs = {{ source = "rules.cc" }}
    outputs = {{ plugin = "rules.so" }}
}}
''', encoding='utf-8')


def prepare(scope, compiler, wrapper, log):
    (scope / 'cap').mkdir()
    (scope / 'profile.h').write_text('// Forced include for this compiler profile.\n', encoding='utf-8')
    write_compiler(wrapper, compiler, log, reject_header=True)
    write_package(scope, wrapper)
    (scope / 'cap/rules.cc').write_text('''\
#include "ycxx_rule_api.h"

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) {
    constexpr char plan[] = R"plan(''' + generator_plan(42) + ''')plan";
    return host->emit(host->context, plan, sizeof(plan) - 1);
}
''', encoding='utf-8')
    (scope / 'build.ycxx').write_text('''\
use plugin("cap/rules") as cap

cap.generator "wire" {}

cpp.executable "app" {
    sources = ["main.cc", artifact("wire", "source")]
}
''', encoding='utf-8')
    (scope / 'main.cc').write_text('int wire_answer(); int main() { return wire_answer(); }\n', encoding='utf-8')


def show(ycxx, scope, env, output_format):
    result = run_client(ycxx, scope, env, 'show', 'app', f'--format={output_format}')
    assert result.returncode == 0, (output_format, result.stderr)
    return result.stdout


def capabilities(ycxx, scope, env, package):
    plan = json.loads(show(ycxx, scope, env, 'json'))
    rows = [row for row in plan['facts'] if (row['kind'], row['target']) == ('toolchain-capability', package)]
    observed = {row['name']: row['value'] for row in rows}
    assert len(observed) == len(rows), rows
    assert all(row['origin'] for row in rows), rows
    assert set(observed.values()) <= {'supported', 'unsupported', 'indeterminate'}, observed
    return observed


def probe(ycxx, scope, env):
    build = run_client(ycxx, scope, env, 'build', 'app', '--probe-toolchain', '--trace-engine=json')
    assert build.returncode == 0, build.stderr[-5000:]
    trace = [json.loads(line) for line in build.stderr.splitlines() if line.strip()]
    assert trace and trace[-1].get('status') == 0, trace[-3:]
    return trace


def probe_actions(trace):
    return [event['key'] for event in trace
            if event.get('kind') in {'ToolchainProbeAction', 'ToolchainProbeLoad', 'ToolchainProbeRun'}
            and event.get('outcome') in {'executed', 'failed'}]


def check_matrix(ycxx, scope, env, compiler):
    wrapper, log = scope / 'pinned-cxx', scope / 'compiler-commands'
    prepare(scope, compiler, wrapper, log)
    assert not capabilities(ycxx, scope, env, PINNED), 'cold show manufactured capabilities'
    assert not log.exists() and not (scope / '.ycxx').exists(), 'cold show ran a compiler or wrote state'

    # Establish a single resident scope and build the real packages and consumer with probing disabled.
    (scope / '.ycxx').mkdir()
    _, baseline = traced_build(ycxx, scope, env, 'ordinary build before capability probing')
    assert not probe_actions(baseline), 'ordinary build ran opt-in probes'
    winners = dict(event['key'].split(' via ', 1) for event in baseline
                   if event.get('kind') == 'RulePackage' and event['outcome'] == 'executed')
    assert set(winners) == {STANDARD, PINNED} and winners[PINNED] == str(wrapper), winners
    if sys.platform == 'linux':
        assert winners[STANDARD] == str(ycxx), winners
    executable = scope / '.ycxx/out/app'
    check_program(executable, 42)
    assert not capabilities(ycxx, scope, env, PINNED), 'ordinary build recorded capability observations'

    first = probe(ycxx, scope, env)
    assert any(event.get('kind') == 'ToolchainProbeResult' for event in first), 'probe results bypassed the engine'
    actions = probe_actions(first)
    for compiler_path in winners.values():
        assert f'driver.default-target via {compiler_path}' in actions, actions
    assert any(event.get('kind') == 'ToolchainProbeLoad'
               and event.get('key') == f'link.host-shared.load via {wrapper}'
               and event.get('outcome') == 'executed' for event in first), actions
    pinned = capabilities(ycxx, scope, env, PINNED)
    standard = capabilities(ycxx, scope, env, STANDARD)
    assert REQUIRED <= pinned.keys() and REQUIRED <= standard.keys(), (pinned, standard)
    for supported in ('driver.default-target', 'dialect.cxx20', 'header.cstddef', 'feature.concepts',
                      'object.default', 'object.host-shared', 'link.hosted', 'link.hosted.run',
                      'link.host-shared', 'link.host-shared.load'):
        assert pinned[supported] == 'supported', (supported, pinned)
    assert pinned['object.riscv64-linux'] == 'indeterminate', pinned

    recorded = log.read_bytes()
    document = html.unescape(show(ycxx, scope, env, 'html'))
    assert all(term in document for term in ('header.cstddef', 'indeterminate', str(wrapper))), document
    assert log.read_bytes() == recorded, 'HTML inspection reran a compiler'

    warm = probe(ycxx, scope, env)
    assert warm[-1]['actions_executed'] == 0, warm[-1]
    assert not probe_actions(warm) and log.read_bytes() == recorded, 'unchanged profile reran probes'
    assert any(event.get('kind') == 'ToolchainProbeResult'
               and event.get('outcome') in {'reused', 'verified', 'durable'} for event in warm), warm[-6:]
    check_program(executable, 42)

    # Flip one question by changing the declared profile, leaving the other provider unchanged.
    write_package(scope, wrapper, PROFILE_FLAG)
    assert not capabilities(ycxx, scope, env, PINNED), 'inspection reported a stale compiler profile'
    assert log.read_bytes() == recorded, 'inspection ran a compiler after flags changed'
    changed = probe_actions(probe(ycxx, scope, env))
    assert f'header.cstddef via {wrapper}' in changed, changed
    assert not any(action.endswith(f' via {winners[STANDARD]}') for action in changed), changed
    assert capabilities(ycxx, scope, env, STANDARD) == standard
    assert capabilities(ycxx, scope, env, PINNED)['header.cstddef'] == 'unsupported'
    assert capabilities(ycxx, scope, env, PINNED)['object.riscv64-linux'] == 'indeterminate'

    # Replace the compiler at the same pin and retain the flags. The new identity must win.
    write_compiler(wrapper, compiler, log, reject_header=False)
    changed = probe_actions(probe(ycxx, scope, env))
    assert f'header.cstddef via {wrapper}' in changed, changed
    assert capabilities(ycxx, scope, env, PINNED)['header.cstddef'] == 'supported'
    recorded = log.read_bytes()
    assert not probe_actions(probe(ycxx, scope, env)) and log.read_bytes() == recorded
    (scope / 'profile.h').write_text('#error profile input changed\n', encoding='utf-8')
    assert not capabilities(ycxx, scope, env, PINNED), 'inspection accepted stale toolchain inputs'
    assert log.read_bytes() == recorded, 'inspection ran probes after a compiler input changed'
    changed = probe_actions(probe(ycxx, scope, env))
    assert f'header.cstddef via {wrapper}' in changed, changed
    assert not any(action.endswith(f' via {winners[STANDARD]}') for action in changed), changed
    assert capabilities(ycxx, scope, env, PINNED)['header.cstddef'] == 'unsupported'
    check_program(executable, 42)


def main():
    ycxx = Path(sys.argv[1]).resolve()
    compiler = Path(sys.argv[2]).absolute()
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-probe-') as directory,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        scope = Path(directory).resolve()
        env = env_for(runtime)
        try:
            check_matrix(ycxx, scope, env, compiler)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
