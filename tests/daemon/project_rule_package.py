"""Run bundled and project-owned C++ rule packages through the resident engine (#1257, #1281)."""

import json
from pathlib import Path
import sys
import tempfile

from project_build import check_program, env_for, run_client, traced_build, tu_outcomes
from runtime_dir import runtime_root


PACKAGE = 'tools/wire-rules'
STANDARD_PACKAGE = 'standard/rules'
PLUGIN_IMPORT = f'use plugin("{PACKAGE}/rules")'
EXECUTABLE = '''cpp.executable "app" {
    sources = ["main.cc", artifact("wire", "source")]
}
'''


def generator_declaration(kind, target, source):
    shell_command = json.dumps("printf '%s\\n' \"$1\" > \"$2\"")
    return f'''\
{kind} "{target}" {{
    tool = "/bin/sh"
    args = ["-c", {shell_command}, "generator", {json.dumps(source)}, output("source")]
    outputs = {{ source = "wire.cc" }}
}}
'''


def generator_plan(value):
    return generator_declaration('action.generate', 'wire', f'int wire_answer() {{ return {value}; }}')


def plugin_source(generation):
    declarations = '\n'.join(
        f'constexpr char plan_{value}[] = R"plan({generator_plan(value)})plan";'
        for value in (7, 11, 17)
    )
    return (r'''
#include "ycxx_rule_api.h"

#include <cstddef>
#include <cstdlib>
#include <string_view>

namespace {

std::string_view text(ycxx_rule_bytes_v1 bytes) {
    return {bytes.data, bytes.size};
}

template<std::size_t Size>
int emit(const ycxx_rule_host_v1* host, const char (&definition)[Size]) {
    return host->emit(host->context, definition, Size - 1);
}

''' + declarations + r'''

constexpr char executable[] = R"plan(native.executable "app" {
    sources = ["replacement.cc"]
})plan";
constexpr std::string_view generation = "@GENERATION@";

}  // namespace

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) {
    if (host->abi_version != YCXX_RULE_ABI_VERSION || host->struct_size < sizeof(*host))
        return 20;
    const std::string_view rule(host->rule);
    const std::string_view target(host->target);
    if (rule == "executable" && target == "app") {
        if (generation == "reject-executable")
            return 26;
        return emit(host, executable);
    }
    if (rule != "generator" || target != "wire")
        return 21;

    ycxx_rule_bytes_v1 bytes{};
    if (host->attribute(host->context, "mode", &bytes) != 1 || text(bytes) != "fixture")
        return 22;
    const int present = host->read(host->context, "settings.txt", &bytes);
    if (present == 0)
        return emit(host, plan_7);
    if (present != 1)
        return 23;

    const auto setting = text(bytes);
    if (setting == generation)
        return 24;
    if (setting == "fail\n")
        std::abort();
    if (setting == "11\n")
        return emit(host, plan_11);
    if (setting == "17\n")
        return emit(host, plan_17);
    return 25;
}
''').replace('@GENERATION@', generation)


def prepare(scope, compiler):
    (scope / '.ycxx').mkdir()
    package = scope / PACKAGE
    package.mkdir(parents=True)
    link_mode = '-dynamiclib' if sys.platform == 'darwin' else '-shared'
    (package / 'build.ycxx').write_text(f'''\
host_action "rules" {{
    tool = {json.dumps(str(compiler))}
    args = ["-std=c++20", "-fPIC", "{link_mode}", "-Iinputs/sdk",
            input("source"), "-o", output("plugin")]
    environment = {{ PATH = "/usr/bin:/bin" }}
    inputs = {{ source = "rules.cc" }}
    outputs = {{ plugin = "rules.so" }}
}}
''', encoding='utf-8')
    (package / 'rules.cc').write_text(plugin_source('edition-one'), encoding='utf-8')
    manifest = PLUGIN_IMPORT + ' as wire\n' + '''\
wire.generator "wire" {
    mode = "fixture"
    reads = ["settings.txt"]
}
''' + EXECUTABLE
    (scope / 'build.ycxx').write_text(manifest, encoding='utf-8')
    (scope / 'main.cc').write_text('int wire_answer();\nint main() { return wire_answer(); }\n', encoding='utf-8')
    (scope / 'replacement.cc').write_text('int main() { return 41; }\n', encoding='utf-8')
    return manifest


def executed_actions(events):
    kinds = {'RulePackage', 'RuleWorker', 'ExternalAction'}
    return [event['kind'] for event in events
            if event.get('kind') in kinds and event.get('outcome') == 'executed']


def check_actions(summary, events, *expected):
    assert summary['actions_executed'] == len(expected), (expected, summary)
    assert sorted(executed_actions(events)) == sorted(expected), (expected, executed_actions(events))


def event_keys(events, kind, outcome='executed'):
    return sorted(event['key'] for event in events
                  if event.get('kind') == kind and event.get('outcome') == outcome)


def check_event_keys(events, kind, *expected, outcome='executed'):
    actual = event_keys(events, kind, outcome)
    assert actual == sorted(expected), (kind, outcome, expected, actual)


def check_standard(ycxx, scope, env):
    manifest = generator_declaration(
        'generate', 'standard_wire',
        'int core_answer(); int wire_answer() { return core_answer() + 29; }',
    ) + '''\
cpp.library "core" {
    sources = ["core.cc"]
}
cpp.executable "app" {
    sources = ["main.cc", artifact("standard_wire", "source")]
    deps = ["core"]
}
'''
    (scope / 'build.ycxx').write_text(manifest, encoding='utf-8')
    (scope / 'core.cc').write_text('int core_answer() { return 3; }\n', encoding='utf-8')
    executable = scope / '.ycxx' / 'out' / 'app'

    summary, events = traced_build(ycxx, scope, env, 'cold bundled provider')
    check_actions(summary, events, 'RulePackage', 'RuleWorker', 'RuleWorker', 'RuleWorker', 'ExternalAction')
    check_event_keys(events, 'RulePackage', STANDARD_PACKAGE)
    check_event_keys(events, 'RuleWorker', 'app', 'core', 'standard_wire')
    check_event_keys(events, 'ExternalAction', 'standard_wire')
    for source in ('main.cc', 'core.cc', 'wire.cc'):
        assert 'executed' in tu_outcomes(events, source), ('bundled provider did not compile', source)
    assert event_keys(events, 'Link'), 'bundled provider did not reach the native linker'
    check_program(executable, 32)
    original = executable.read_bytes()

    summary, events = traced_build(ycxx, scope, env, 'warm bundled provider')
    check_actions(summary, events)
    assert not event_keys(events, 'Link'), 'warm bundled provider linked again'
    assert executable.read_bytes() == original, 'warm bundled provider changed the output'


def check_private_primitives(ycxx, scope, env):
    native_library = '''\
native.library "core" {
    sources = ["core.cc"]
}
cpp.executable "app" {
    sources = ["replacement.cc"]
    deps = ["core"]
}
'''
    native_executable = '''\
native.executable "app" {
    sources = ["replacement.cc"]
}
'''
    external_action = generator_plan(7) + EXECUTABLE
    for kind, manifest in [('native.executable', native_executable),
                           ('native.library', native_library),
                           ('action.generate', external_action)]:
        (scope / 'build.ycxx').write_text(manifest, encoding='utf-8')
        summary, events = traced_build(ycxx, scope, env, f'direct {kind}', succeeds=False)
        assert any(f"primitive '{kind}' can only be emitted by a rule" in diagnostic
                   for diagnostic in summary['diagnostics']), summary
        assert not event_keys(events, 'Link'), (kind, 'a direct primitive reached the native linker')
        assert not event_keys(events, 'ExternalAction'), (kind, 'a direct primitive ran a tool')


def check_package(ycxx, scope, env, compiler):
    manifest = prepare(scope, compiler)
    check_standard(ycxx, scope, env)
    (scope / 'build.ycxx').write_text(manifest, encoding='utf-8')
    executable = scope / '.ycxx' / 'out' / 'app'
    settings = scope / 'settings.txt'

    summary, events = traced_build(ycxx, scope, env, 'cold rule package')
    check_actions(summary, events, 'RulePackage', 'RuleWorker', 'RuleWorker', 'ExternalAction')
    check_event_keys(events, 'RulePackage', f'{PACKAGE}/rules')
    check_event_keys(events, 'RuleWorker', 'app', 'wire')
    assert event_keys(events, 'Link'), 'standard app and custom generator did not reach the native linker'
    check_program(executable, 7)
    initial = executable.read_bytes()

    summary, events = traced_build(ycxx, scope, env, 'warm rule package')
    check_actions(summary, events)
    assert executable.read_bytes() == initial, 'warm build changed the output'

    (scope / PACKAGE / 'rules.cc').write_text(plugin_source('edition-two'), encoding='utf-8')
    summary, events = traced_build(ycxx, scope, env, 'new library, equivalent rule')
    check_actions(summary, events, 'RulePackage', 'RuleWorker')
    assert executable.read_bytes() == initial, 'equivalent expansion changed the executable'

    settings.write_text('11\n', encoding='utf-8')
    summary, events = traced_build(ycxx, scope, env, 'create previously missing tracked input')
    check_actions(summary, events, 'RuleWorker', 'ExternalAction')
    assert 'executed' in tu_outcomes(events, 'wire.cc'), 'new rule output did not compile'
    check_program(executable, 11)
    before_failure = executable.read_bytes()

    settings.write_text('fail\n', encoding='utf-8')
    summary, events = traced_build(ycxx, scope, env, 'worker failure', succeeds=False)
    assert summary['diagnostics'], 'a failing rule must explain the failure'
    assert 'ExternalAction' not in executed_actions(events), 'a failed rule ran its generator'
    assert executable.read_bytes() == before_failure, 'a failed rule replaced the last successful output'
    check_program(executable, 11)

    settings.write_text('17\n', encoding='utf-8')
    summary, events = traced_build(ycxx, scope, env, 'worker recovery')
    check_actions(summary, events, 'RuleWorker', 'ExternalAction')
    check_program(executable, 17)

    # Keep the cpp.executable declaration byte for byte and replace only its provider.
    (scope / 'build.ycxx').write_text(PLUGIN_IMPORT + ' as cpp\n' + manifest, encoding='utf-8')
    summary, events = traced_build(ycxx, scope, env, 'explicit replacement of the cpp provider')
    check_actions(summary, events, 'RuleWorker')
    check_event_keys(events, 'RuleWorker', 'app')
    assert 'executed' in tu_outcomes(events, 'replacement.cc'), 'cpp override did not select its native source'
    assert event_keys(events, 'Link'), 'cpp override did not reach the native linker'
    check_program(executable, 41)
    replaced = executable.read_bytes()

    summary, events = traced_build(ycxx, scope, env, 'warm explicit provider')
    check_actions(summary, events)

    (scope / PACKAGE / 'rules.cc').write_text(plugin_source('reject-executable'), encoding='utf-8')
    summary, events = traced_build(ycxx, scope, env, 'failing explicit provider', succeeds=False)
    assert summary['actions_executed'] == 2, summary
    check_event_keys(events, 'RulePackage', f'{PACKAGE}/rules')
    check_event_keys(events, 'RuleWorker', 'app', outcome='failed')
    assert not event_keys(events, 'RuleWorker'), 'explicit provider failure fell back to another worker'
    assert not event_keys(events, 'Link'), 'explicit provider failure reached the native linker'
    assert any('status 26' in diagnostic for diagnostic in summary['diagnostics']), summary
    assert executable.read_bytes() == replaced, 'explicit provider failure changed the executable'
    check_program(executable, 41)

    check_private_primitives(ycxx, scope, env)


def main():
    ycxx = Path(sys.argv[1]).resolve()
    compiler = Path(sys.argv[2]).absolute()
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-rule-package-') as directory,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        scope = Path(directory).resolve()
        env = env_for(runtime)
        try:
            check_package(ycxx, scope, env, compiler)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
