"""Generate and incrementally consume a typed capability header (#1290)."""

import json
from pathlib import Path
import shlex
import sys
import tempfile

from project_build import action_keys, check_program, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_toolchain_probe import probe_actions, show
from runtime_dir import runtime_root


PACKAGE = 'tools/host/rules'
PACKAGE_MANIFEST = 'tools/host/build.ycxx'
EQUIVALENT_FLAG = '-DYCXX_PROBE_EQUIVALENT=1'
REJECT_FILESYSTEM_FLAG = '-DYCXX_PROBE_REJECT_FILESYSTEM=1'
BINDINGS = {
    'filesystem': 'header.filesystem',
    'no_header': 'header.cstddef',
    'no_object': 'object.riscv64-linux',
}

MANIFEST = f'''\
use plugin("builtin:capabilities") as capabilities

capabilities.header "profile" {{
    namespace = "project::compiler"
    filename = "compiler_profile.hh"
    capabilities = {{
        filesystem = probe("{PACKAGE}", "{BINDINGS['filesystem']}")
        no_header = probe("{PACKAGE}", "{BINDINGS['no_header']}")
        no_object = probe("{PACKAGE}", "{BINDINGS['no_object']}")
    }}
}}

cpp.executable "app" {{
    sources = ["main.cc"]
    generated_headers = [artifact("profile", "header")]
}}
'''

CONSUMER = '''\
#include "compiler_profile.hh"

namespace cc = project::compiler;
static_assert(cc::no_header == cc::support::unsupported);
static_assert(cc::no_object == cc::support::indeterminate);
static_assert(!cc::available<cc::no_header>);
static_assert(!cc::available<cc::no_object>);

template<cc::support State>
    requires cc::available<State>
constexpr int selected() { return 42; }

template<cc::support State>
constexpr int dispatch() {
    if constexpr (cc::available<State>) {
        return selected<State>();
    } else {
        return 17;
    }
}

int main() { return dispatch<cc::filesystem>(); }
'''

CUSTOM_MANIFEST = f'''\
use plugin("{PACKAGE}") as custom

custom.header "profile" {{
    capabilities = {{ filesystem = probe("{PACKAGE}", "{BINDINGS['filesystem']}") }}
}}

cpp.executable "app" {{
    sources = ["main.cc"]
    generated_headers = [artifact("profile", "header")]
}}
'''

CUSTOM_RULE = r'''
#include "ycxx_rule_api.h"

#include <string_view>

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) {
    if (host->struct_size < sizeof(*host) || host->capability_count != 1 || !host->capabilities)
        return 22;
    const auto& capability = host->capabilities[0];
    if (std::string_view(capability.name) != "filesystem")
        return 23;
    if (capability.support == YCXX_RULE_UNSUPPORTED_V1)
        return 24;
    if (capability.support != YCXX_RULE_SUPPORTED_V1)
        return 25;

    constexpr char plan[] = R"plan(action.generate "profile" {
        tool = "/bin/sh"
        args = ["-c", "printf '%s' \"$1\" > \"$2\"", "generator",
                "inline constexpr int answer = 42;", output("header")]
        outputs = { header = "compiler_profile.hh" }
    })plan";
    return host->emit(host->context, plan, sizeof(plan) - 1);
}
'''


def package_definition(wrapper, *flags, probe_inputs=()):
    shared = '-dynamiclib' if sys.platform == 'darwin' else '-shared'
    return f'''\
host_action "rules" {{
    tool = cxx({json.dumps(str(wrapper))})
    args = ["-std=c++20", "-fPIC", "{shared}", "-Iinputs/sdk",
            input("source"), "-o", output("plugin")]
    probe_flags = {json.dumps(flags)}
    probe_inputs = {json.dumps(probe_inputs)}
    environment = {{ PATH = "/usr/bin:/bin" }}
    inputs = {{ source = "rules.cc" }}
    outputs = {{ plugin = "rules.so" }}
}}
'''


def write_compiler(wrapper, compiler, log, *, mode=None):
    # Delegate real compilation to the pinned compiler. Inject one rejection and
    # one clean exit without an object, independently of the host and driver.
    wrapper.write_text(f'''\
#!/bin/sh
printf '%s\\n' "$*" >> {shlex.quote(str(log))}
mode={shlex.quote(str(mode)) if mode else "''"}
reject=0
header=
for arg do
    case "$arg" in
        {REJECT_FILESYSTEM_FLAG}) reject=1 ;;
        --target=riscv64-unknown-linux-gnu) exit 0 ;;
        *.cc|*.cpp)
            if /usr/bin/grep -Fq '#include <cstddef>' "$arg"; then header=cstddef; fi
            if /usr/bin/grep -Fq '#include <filesystem>' "$arg"; then header=filesystem; fi ;;
    esac
done
if [ -n "$mode" ] && /usr/bin/grep -Fxq disabled "$mode"; then reject=1; fi
if [ "$header" = cstddef ] || {{ [ "$header" = filesystem ] && [ "$reject" = 1 ]; }}; then
    printf '%s\\n' "fixture: $header rejected by compiler profile" >&2
    exit 1
fi
exec {shlex.quote(str(compiler))} "$@"
''', encoding='utf-8')
    wrapper.chmod(0o700)


def prepare(scope, compiler, wrapper, log):
    (scope / 'tools/host').mkdir(parents=True)
    (scope / PACKAGE_MANIFEST).write_text(package_definition(wrapper), encoding='utf-8')
    (scope / 'tools/host/rules.cc').write_text('''\
#include "ycxx_rule_api.h"

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1*) { return 0; }
''', encoding='utf-8')
    (scope / 'build.ycxx').write_text(MANIFEST, encoding='utf-8')
    (scope / 'main.cc').write_text(CONSUMER, encoding='utf-8')
    write_compiler(wrapper, compiler, log)


def profile_header(ycxx, scope, env, wrapper, filesystem):
    plan = json.loads(show(ycxx, scope, env, 'json'))
    facts = plan['facts']
    projected = [row for row in facts
                 if (row['kind'], row['target']) == ('capability-input', 'profile')]
    expected = {'filesystem': filesystem, 'no_header': 'unsupported', 'no_object': 'indeterminate'}
    assert len(projected) == len(expected), projected
    assert {row['name']: row['value'] for row in projected} == expected, projected
    for row in projected:
        origin = row['origin']
        assert BINDINGS[row['name']] in origin and wrapper.name in origin, row
        assert 'profile' in origin.lower(), row
    assert plan['dependency_graph']['value_edges'] == [], 'package-only probes have no producer target'
    assert any((row['kind'], row['target'], row['name']) == ('output', 'profile', 'header')
               and row['value'].endswith('/compiler_profile.hh') for row in facts), facts
    assert any(row['kind'] == 'include' and row['target'] == 'app'
               and row['origin'] == 'artifact("profile", "header")' for row in facts), facts

    # The generator publishes the literal argument as a virtual artifact. Use
    # the inspected argument to compare its exact bytes across profile changes.
    header = [row['value'] for row in facts
              if row['kind'] == 'argv' and row['target'] == 'profile'
              and 'namespace project::compiler' in row['value']]
    assert len(header) == 1, header
    return header[0].encode('utf-8')


def check_demand(events, wrapper, *, cold=False):
    actions = probe_actions(events)
    for probe in BINDINGS.values():
        assert f'{probe} via {wrapper}' in actions, (probe, actions)
    assert not any(action.startswith(('dialect.', 'link.', 'feature.')) for action in actions), actions
    results = [event['key'] for event in events if event.get('kind') == 'ToolchainProbeResult']
    assert all(any(probe in key for key in results) for probe in BINDINGS.values()), results
    if cold:
        assert any(event.get('kind') == 'CapabilityProjection' and event.get('key', '').startswith('profile')
                   for event in events), events


def check_generation(events):
    assert 'profile' in action_keys(events, 'RuleWorker'), events
    assert action_keys(events, 'ExternalAction') == ['profile'], events
    assert 'executed' in tu_outcomes(events, 'main.cc'), events
    assert action_keys(events, 'Link'), events


def check_rejected(summary, events, spelling, executable, previous):
    assert any(spelling in diagnostic for diagnostic in summary['diagnostics']), summary
    assert not any(event.get('kind') in {'ExternalAction', 'Link'} for event in events), events
    assert executable.read_bytes() == previous, 'invalid header declaration replaced the executable'


def check_header(ycxx, scope, env, compiler):
    wrapper, log = scope / 'pinned-cxx', scope / 'compiler-commands'
    prepare(scope, compiler, wrapper, log)
    facts = json.loads(show(ycxx, scope, env, 'json'))['facts']
    assert not any(row['kind'] == 'capability-input' for row in facts), facts
    assert not log.exists() and not (scope / '.ycxx').exists(), 'cold show executed project actions'

    executable = scope / '.ycxx/out/app'
    _, events = traced_build(ycxx, scope, env, 'ordinary build with demanded capabilities')
    check_demand(events, wrapper, cold=True)
    check_generation(events)
    check_program(executable, 42)
    recorded = log.read_bytes()
    original_header = profile_header(ycxx, scope, env, wrapper, 'supported')
    assert log.read_bytes() == recorded, 'inspection ran compiler probes'
    original_program = executable.read_bytes()
    installed_at = executable.stat().st_mtime_ns

    (scope / PACKAGE_MANIFEST).write_text(package_definition(wrapper, EQUIVALENT_FLAG), encoding='utf-8')
    _, events = traced_build(ycxx, scope, env, 'reprobe with equivalent capabilities')
    check_demand(events, wrapper)
    assert 'profile' not in action_keys(events, 'RuleWorker'), events
    assert not action_keys(events, 'ExternalAction') and not action_keys(events, 'Link'), events
    assert 'executed' not in tu_outcomes(events, 'main.cc'), events
    assert profile_header(ycxx, scope, env, wrapper, 'supported') == original_header
    assert executable.read_bytes() == original_program
    assert executable.stat().st_mtime_ns == installed_at, 'equivalent capabilities replaced the executable'

    (scope / PACKAGE_MANIFEST).write_text(
        package_definition(wrapper, EQUIVALENT_FLAG, REJECT_FILESYSTEM_FLAG), encoding='utf-8')
    _, events = traced_build(ycxx, scope, env, 'reprobe with changed support')
    check_demand(events, wrapper)
    check_generation(events)
    assert profile_header(ycxx, scope, env, wrapper, 'unsupported') != original_header
    check_program(executable, 17)
    previous = executable.read_bytes()

    replace_source(scope, 'build.ycxx', MANIFEST.replace('project::compiler', 'project::bad-name'))
    summary, events = traced_build(ycxx, scope, env, 'invalid namespace', succeeds=False)
    check_rejected(summary, events, 'bad-name', executable, previous)
    replace_source(scope, 'build.ycxx', MANIFEST)

    watch = Watch(ycxx, scope, env)
    try:
        _, _, watched = watch.build('watch current capabilities')
        assert str(scope / PACKAGE_MANIFEST) in watched, watched
        replace_source(scope, 'build.ycxx', MANIFEST.replace('no_header =', 'available ='))
        summary, events, _ = watch.build('reserved capability name', succeeds=False)
        check_rejected(summary, events, 'available', executable, previous)
        check_program(executable, 17)

        replace_source(scope, 'build.ycxx', MANIFEST)
        watch.build('recover after invalid capability rule')
        check_program(executable, 17)

        replace_source(scope, PACKAGE_MANIFEST, package_definition(wrapper))
        _, events, _ = watch.build('watch changed support after recovery')
        # Returning to a previously seen profile can reuse both probes and the
        # generator receipt. The dependent translation unit must still change.
        assert 'executed' in tu_outcomes(events, 'main.cc'), events
        check_program(executable, 42)
        assert profile_header(ycxx, scope, env, wrapper, 'supported') == original_header
        watch.idle('output publication and inspection do not rebuild')
    finally:
        watch.close()


def check_probe_input_recovery(ycxx, scope, env, compiler):
    (scope / 'tools/host').mkdir(parents=True)
    mode = scope / 'tools/host/mode.txt'
    mode.write_text('disabled\n', encoding='utf-8')
    wrapper = scope / 'pinned-cxx'
    write_compiler(wrapper, compiler, scope / 'compiler-commands', mode=mode)
    (scope / PACKAGE_MANIFEST).write_text(
        package_definition(wrapper, probe_inputs=('mode.txt',)), encoding='utf-8')
    (scope / 'tools/host/rules.cc').write_text(CUSTOM_RULE, encoding='utf-8')
    (scope / 'build.ycxx').write_text(CUSTOM_MANIFEST, encoding='utf-8')
    (scope / 'main.cc').write_text(
        '#include "compiler_profile.hh"\nint main() { return answer; }\n', encoding='utf-8')

    executable = scope / '.ycxx/out/app'
    watch = Watch(ycxx, scope, env)
    try:
        summary, events, watched = watch.build('cold watch with unsupported capability', succeeds=False)
        assert any('failed with status 24' in diagnostic for diagnostic in summary['diagnostics']), summary
        assert action_keys(events, 'RuleWorker', 'failed') == ['profile'], events
        assert f"header.filesystem via {wrapper}" in probe_actions(events), events
        assert str(mode) in watched, watched
        assert not executable.exists()
        watch.idle('failed custom rule awaits a compiler input change')

        replace_source(scope, mode.relative_to(scope), 'enabled\n')
        _, events, _ = watch.build('recover after editing only a declared compiler input')
        assert f"header.filesystem via {wrapper}" in probe_actions(events), events
        check_generation(events)
        check_program(executable, 42)
    finally:
        watch.close()


def main():
    ycxx = Path(sys.argv[1]).resolve()
    compiler = Path(sys.argv[2]).absolute()
    for check in (check_header, check_probe_input_recovery):
        with (
            tempfile.TemporaryDirectory(prefix='ycxx-capability-header-') as directory,
            tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
        ):
            checkout = Path(directory).resolve()
            (checkout / '.git').mkdir()
            scope = checkout / 'project'
            scope.mkdir()
            env = env_for(runtime)
            try:
                check(ycxx, scope, env, compiler)
            finally:
                for root in (scope, checkout):
                    run_client(ycxx, root, env, 'ctl', 'shutdown', '--root', str(root))


if __name__ == '__main__':
    main()
