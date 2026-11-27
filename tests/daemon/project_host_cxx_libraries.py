"""Share a disk-backed library diamond between a host tool and a foreign app (#1576)."""

from collections import Counter
import html
import json
from pathlib import Path
import sys
import tempfile

from project_build import action_keys, env_for, run_client, traced_build, tu_outcomes
from project_build_watch import Watch, replace_source
from project_inspection import (
    check_dependency_changes_html, check_dependency_graph, check_dependency_html, check_location,
    check_target_dependencies_html, dependency_edge, fact, scope_state,
)
from project_native_host_tool import GENERATED_HEADER, NOTE, REFERENCE, SCANNER, prepare as prepare_tool
from project_shader_bundle import check_external_actions
from project_targets import (
    DARWIN, LINUX, RAN, build, check_binary, check_target_trace, executable, journal_contents,
    published, target_args,
)
from runtime_dir import runtime_root


COMMON = 'support/common.cc'
LEFT = 'support/left.cc'
RIGHT = 'support/right.cc'
VALUE_HEADER = 'include/score_value.hh'
REPAIR = 'include/repair_score.hh'
LIBRARIES = f'''\
cpp.library "left" {{
    sources = ["{LEFT}"]
    deps = ["common"]
}}
cpp.library "right" {{
    sources = ["{RIGHT}"]
    deps = ["common"]
}}
cpp.library "common" {{
    sources = ["{COMMON}"]
    public_include_dirs = ["include"]
}}
'''


def score_header(value):
    return f'#pragma once\ninline constexpr int shared_score = {value};\n'


def prepare(scope, journal):
    definition = prepare_tool(scope, journal)
    for directory in ('include', 'support'):
        (scope / directory).mkdir()
    (scope / 'include/score_api.hh').write_text('''\
#pragma once
#if defined(__x86_64__)
inline constexpr int compile_arch = 1;
#elif defined(__aarch64__)
inline constexpr int compile_arch = 2;
#else
#error unexpected target architecture
#endif
int common_arch();
int common_score();
int left_score();
int right_score();
''', encoding='utf-8')
    (scope / VALUE_HEADER).write_text(score_header(17), encoding='utf-8')
    (scope / COMMON).write_text('''\
#include "score_api.hh"
#include "score_value.hh"
int common_arch() { return compile_arch; }
int common_score() { return shared_score; }
''', encoding='utf-8')
    for path, name, offset in ((LEFT, 'left_score', 1), (RIGHT, 'right_score', 2)):
        (scope / path).write_text(f'#include "score_api.hh"\nint {name}() {{ return common_score() + {offset}; }}\n',
                                 encoding='utf-8')

    (scope / SCANNER).write_text('''\
#include "score_api.hh"
int main() {
    if (common_arch() != compile_arch) return 2;
    return left_score() + right_score() - common_score() - 3;
}
''', encoding='utf-8')
    (scope / 'main.cc').write_text('''\
#include "score_api.hh"
#include "scan_score.hh"
int main() {
    if (common_arch() != compile_arch) return 2;
    int linked = left_score() + right_score() - common_score() - 3;
    return linked == scanned_score ? linked : 1;
}
''', encoding='utf-8')
    (scope / 'scanner_score.hh').unlink()

    for source in (SCANNER, 'main.cc'):
        sources = f'sources = ["{source}"]'
        assert definition.count(sources) == 1, definition
        definition = definition.replace(sources, f'{sources}\n    deps = ["left", "right"]')
    definition += '\n' + LIBRARIES
    (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
    return definition


def inspect(ycxx, scope, env, journal, other, host, *, cold=False):
    before = scope_state(scope)
    recorded = journal_contents(journal)
    args = ('show', 'app', *target_args(other))
    result = run_client(ycxx, scope, env, *args, '--format=json')
    assert result.returncode == 0, result.stderr
    plan = json.loads(result.stdout)
    assert (plan['schema'], plan['target']) == (1, 'app'), plan
    target, = (row for row in plan['facts'] if row['kind'] == 'native-target')
    assert (target['target'], target['name'], target['value']) == ('app', 'triple', other), target
    if cold:
        check_dependency_graph(plan, [], complete=False)
        assert not (scope / '.ycxx').exists(), 'cold show created build state'
    else:
        definition = plan['definition']
        host_fact = fact(plan, 'host-target', 'scanner', host)
        assert host_fact['name'] == 'triple', host_fact
        check_location(host_fact, definition, 'cpp.host_executable "scanner"')
        for name, source in (('left', LEFT), ('right', RIGHT), ('common', COMMON)):
            check_location(fact(plan, 'source', name, source), definition, json.dumps(source))
        keys = Counter((row['kind'], row['target'], row['name']) for row in plan['facts'])
        assert all(count == 1 for count in keys.values()), [key for key, count in keys.items() if count != 1]
        deps = (
            ('app', 'left', 'cpp.executable'), ('app', 'right', 'cpp.executable'),
            ('scanner', 'left', 'cpp.host_executable'), ('scanner', 'right', 'cpp.host_executable'),
            ('left', 'common', 'cpp.library'), ('right', 'common', 'cpp.library'),
        )
        edges = [
            dependency_edge(definition, 'app', 'scan_config', 'generated_headers', GENERATED_HEADER, output='header'),
            dependency_edge(definition, 'app', 'scan_config', 'runtime_assets', NOTE, output='note'),
            dependency_edge(definition, 'scan_config', 'scanner', 'inputs', REFERENCE,
                            output='executable', binding='scanner'),
            *(dependency_edge(definition, consumer, producer, 'deps', json.dumps(producer),
                              start=definition.index(f'{kind} "{consumer}"'))
              for consumer, producer, kind in deps),
        ]
        check_dependency_graph(plan, edges)
        result = run_client(ycxx, scope, env, *args, '--format=html')
        assert result.returncode == 0, result.stderr
        visible = html.unescape(result.stdout)
        assert all(text in visible for text in (host, other, REFERENCE, COMMON)), visible
        check_dependency_html(result.stdout, plan)
        check_target_dependencies_html(result.stdout, plan)
    assert scope_state(scope) == before and journal_contents(journal) == recorded, 'show ran a project action'
    return plan


def compare(ycxx, scope, env, journal, other, plan):
    baseline = scope.with_name('library-baseline.json')
    baseline.write_text(json.dumps(plan), encoding='utf-8')
    before = scope_state(scope)
    recorded = journal_contents(journal)
    args = ('show', 'app', *target_args(other), '--against', str(baseline))
    result = run_client(ycxx, scope, env, *args, '--format=json')
    assert result.returncode == 0, result.stderr
    diff = json.loads(result.stdout)
    assert diff['changes'] == diff['dependency_changes'] == [], diff
    result = run_client(ycxx, scope, env, *args, '--format=html')
    assert result.returncode == 0, result.stderr
    check_dependency_changes_html(result.stdout, plan['definition'], [])
    check_target_dependencies_html(result.stdout, plan)
    assert scope_state(scope) == before and journal_contents(journal) == recorded, 'comparison ran a project action'


def check_watch(ycxx, scope, env, journal, host, other):
    default = published(scope, None)
    initial = published(scope, other)
    recorded = journal_contents(journal)
    watch = Watch(ycxx, scope, env, *target_args(other))
    try:
        summary, events, watched = watch.build('unchanged host and foreign library graphs')
        assert summary['actions_executed'] == 0, summary
        assert {str(scope / path) for path in (SCANNER, COMMON, VALUE_HEADER)} <= set(watched), watched
        check_external_actions(events)

        replace_source(scope, VALUE_HEADER, '#include "repair_score.hh"\n')
        summary, events, watched = watch.build('host library discovers a missing include', succeeds=False)
        assert any("host executable 'scanner'" in item and 'file not found' in item
                   for item in summary['diagnostics']), summary
        assert str(scope / VALUE_HEADER) in watched, watched
        check_external_actions(events)
        assert journal_contents(journal) == recorded
        assert published(scope, None) == default and published(scope, other) == initial

        replace_source(scope, REPAIR, score_header(23))
        _, events, watched = watch.build('repair the shared library include in one edit')
        assert str(scope / REPAIR) in watched, watched
        check_target_trace(events, other)
        check_external_actions(events, 'scan_config')
        assert 'executed' in tu_outcomes(events, 'main.cc'), events
        check_binary(scope, other, other, 23, host)
        assert journal_contents(journal) == recorded + '23\n'
        assert published(scope, None) == default and published(scope, other)[0][0] != initial[0][0]
        inspect(ycxx, scope, env, journal, other, host)
        watch.idle('watch after repairing the host library include')
    finally:
        watch.close()


def check_builds(ycxx, scope, env, journal, host, other):
    prepare(scope, journal)
    inspect(ycxx, scope, env, journal, other, host, cold=True)
    _, events = build(ycxx, scope, env, other, other, 'build foreign app and native scanner with a shared diamond')
    check_external_actions(events, 'scan_config')
    assert 'executed' in tu_outcomes(events, SCANNER) and 'executed' in tu_outcomes(events, 'main.cc'), events
    units = Counter()
    for event in events:
        if event.get('kind') == 'Tu' and event['outcome'] == 'executed':
            units[Path(event['key']).name] += event.get('repeat', 1)
    # Each library TU belongs to one host and one foreign compile, despite two routes to common.
    assert {Path(source).name: units[Path(source).name] for source in (COMMON, LEFT, RIGHT)} == {
        'common.cc': 2, 'left.cc': 2, 'right.cc': 2,
    }, units
    check_binary(scope, other, other, 17, host)
    assert journal_contents(journal) == '17\n', 'the foreign build did not execute the scanner for the host'
    plan = inspect(ycxx, scope, env, journal, other, host)
    compare(ycxx, scope, env, journal, other, plan)

    _, events = build(ycxx, scope, env, None, host, 'reuse the host scanner on the default target')
    check_external_actions(events)
    assert not RAN & tu_outcomes(events, SCANNER), events
    check_binary(scope, None, host, 17, host)
    assert journal_contents(journal) == '17\n'

    installed = published(scope, other)
    replace_source(scope, VALUE_HEADER, score_header(17) + '// Same host image.\n')
    inspect(ycxx, scope, env, journal, other, host)
    _, events = build(ycxx, scope, env, other, other, 'reuse selected output after an equal host image')
    check_external_actions(events)
    assert published(scope, other) == installed and journal_contents(journal) == '17\n'

    scanner = (scope / SCANNER).read_text(encoding='utf-8')
    entry = 'int main() {\n'
    assert scanner.count(entry) == 1, scanner
    edited = scanner.replace(entry, 'int main(int argc, char**) {\n    if (argc > 1) return 41;\n')
    replace_source(scope, SCANNER, edited)
    _, events = build(ycxx, scope, env, other, other, 'reuse the app after changed scanner bytes return the same score')
    assert 'executed' in tu_outcomes(events, SCANNER), events
    check_external_actions(events, 'scan_config')
    assert not RAN & tu_outcomes(events, 'main.cc'), events
    assert published(scope, other) == installed and journal_contents(journal) == '17\n17\n'
    check_watch(ycxx, scope, env, journal, host, other)

    _, events = build(ycxx, scope, env, None, host, 'link the repaired library under the default target')
    check_external_actions(events)
    check_binary(scope, None, host, 23, host)
    assert journal_contents(journal) == '17\n17\n23\n'


def check_rejections(ycxx, workspace, env, other):
    sources = f'sources = ["{COMMON}"]'
    for name, replacement, diagnostic in (
        ('cycle', f'{sources}\n    deps = ["left"]', "dependency cycle at 'left'"),
        ('generated-archive', f'{sources}\n    public_link_inputs = [artifact("blocked", "archive")]',
         'public_link_inputs is not supported in host executable dependencies'),
    ):
        scope, journal = workspace / name, workspace / f'{name}-runs'
        definition = prepare(scope, journal)
        # Keep the invalid library reachable only from the host side of this variant.
        start = definition.index('cpp.executable "app"')
        definition = definition[:start] + definition[start:].replace('    deps = ["left", "right"]\n', '', 1)
        assert definition.count(sources) == 1, definition
        definition = definition.replace(sources, replacement) + '''\
generate "blocked" {
    tool = "/bin/false"
    outputs = { source = "blocked.cc" header = "blocked.hh" archive = "blocked.a" }
}
'''
        (scope / 'build.ycxx').write_text(definition, encoding='utf-8')
        try:
            summary, events = traced_build(ycxx, scope, env, f'reject {name} from a host library',
                                           *target_args(other), succeeds=False)
            assert any(diagnostic in item for item in summary['diagnostics']), summary
            check_external_actions(events)
            assert not action_keys(events, 'Link') and not RAN & tu_outcomes(events, SCANNER), events
            assert not journal.exists() and not (scope / executable(other)).exists()
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


def main():
    ycxx = Path(sys.argv[1]).resolve()
    host, other = {'linux': (LINUX, DARWIN), 'darwin': (DARWIN, LINUX)}[sys.platform]
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-host-cxx-libraries-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        root = Path(workspace).resolve()
        scope, journal = root / 'project', root / 'scanner-runs'
        env = env_for(runtime)
        try:
            check_builds(ycxx, scope, env, journal, host, other)
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')
        check_rejections(ycxx, root, env, other)


if __name__ == '__main__':
    main()
