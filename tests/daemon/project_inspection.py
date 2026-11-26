"""Inspect source, resolved provenance, build diffs, and workspace targets (#1256, #1392, #1441, #1453)."""

import html
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from project_build import daemon_identity, env_for, run_client, traced_build
from runtime_dir import runtime_root


MARKUP = '<script>alert("definition")</script> & report'
UNSAFE_SOURCE = 'src/literal <b>&".cc'
UNSAFE_TARGET = 'group/<script data-catalog-probe="1">&\tleaf</script>'
CORE = 'core <b data-dependency-probe="1">&'
INSPECTION_TARGETS = [
    ('app', 'cpp.executable'), (CORE, 'cpp.library'), ('wire', 'generate'), ('wire_schema', 'generate'),
]


def prepare(scope):
    for directory in ('src', 'auto', 'include', 'fallback'):
        (scope / directory).mkdir()
    (scope / 'src/main.cc').write_text('int main() { return 0; }\n', encoding='utf-8')
    for index, source in enumerate((UNSAFE_SOURCE, 'src/core.cc', 'auto/first.cc')):
        (scope / source).write_text(f'int fixture_{index}() {{ return {index}; }}\n', encoding='utf-8')

    generator = scope / 'generator.sh'
    generator.write_text(r'''#!/bin/sh
printf ran > "$1" || exit 5
printf 'int generated_wire() { return 0; }\n' > "$3" || exit 5
printf '\n' > "$4"
''', encoding='utf-8')
    generator.chmod(0o700)
    (scope / 'schema.txt').write_text('schema\n', encoding='utf-8')

    # Keep the CRLF on the original comment and omit the final newline.
    return f'''# {MARKUP}\r
generate "wire_schema" {{
    tool = "/bin/cp"
    args = [input("file"), output("schema")]
    inputs = {{ file = "schema.txt" }}
    outputs = {{ schema = "schema.txt" }}
}}
generate "wire" {{
    tool = {json.dumps(str(generator))}
    args = [{json.dumps(str(scope / 'generator-ran'))}, input("schema"), output("source"), output("header")]
    inputs = {{ schema = artifact("wire_schema", "schema") }}
    outputs = {{ source = "wire.cc" header = "wire.h" }}
}}
cpp.library {json.dumps(CORE)} {{
    sources = ["src/core.cc"]
    public_include_dirs = ["include", "fallback"]
}}
cpp.executable "app" {{
    sources = ["src/main.cc", {json.dumps(UNSAFE_SOURCE)}, glob("auto/*.cc"), artifact("wire", "source")]
    generated_headers = [artifact("wire", "header")]
    deps = [{json.dumps(CORE)}]
}}'''


def show(ycxx, scope, *args):
    result = subprocess.run([ycxx, 'show', 'app', *args], cwd=scope, capture_output=True, timeout=20)
    assert result.returncode == 0, (args, result.returncode, result.stderr.decode('utf-8', errors='replace'))
    return result.stdout


def show_json(ycxx, scope, *args):
    obj = json.loads(show(ycxx, scope, '--format=json', *args))
    assert obj['schema'] == 1 and obj['target'] == 'app', obj
    return obj


def fact(snapshot, kind, target, value):
    found = [item for item in snapshot['facts']
             if (item['kind'], item['target'], item['value']) == (kind, target, value)]
    assert len(found) == 1, (kind, target, value, found)
    item = found[0]
    assert item['name'] and item['origin'], item
    assert type(item['line']) is int and type(item['column']) is int, item
    assert item['line'] > 0 and item['column'] > 0, item
    return item


def manifest_location(definition, start):
    line = definition.count('\n', 0, start) + 1
    previous = definition.rfind('\n', 0, start)
    column = len(definition[previous + 1:start].encode('utf-8')) + 1
    return {'path': 'build.ycxx', 'line': line, 'column': column}


def check_location(item, definition, spelling):
    location = manifest_location(definition, definition.index(spelling))
    assert (item['line'], item['column']) == (location['line'], location['column']), (spelling, location, item)


def dependency_edge(definition, consumer, producer, usage, spelling, *, start=0, output=None, binding=None):
    edge = {'consumer': consumer, 'producer': producer, 'usage': usage,
            'location': manifest_location(definition, definition.index(spelling, start))}
    if output is not None:
        edge['output'] = output
    if binding is not None:
        edge['binding'] = binding
    return edge


def check_dependency_graph(plan, edges, *, complete=True):
    def order(edge):
        location = edge['location']
        return (edge['consumer'], location['line'], location['column'], edge['usage'], edge['producer'],
                edge.get('output', ''), edge.get('binding', ''))

    assert 'dependency_graph' in plan, 'selected plan omitted the dependency graph'
    expected = {'complete': complete, 'edges': sorted(edges, key=order)}
    assert plan['dependency_graph'] == expected, (plan['dependency_graph'], expected)
    assert plan['dependency_graph']['complete'] is complete, plan['dependency_graph']


def check_dependency_html(document, plan):
    # Module discovery also imports the plan fact checks here.
    from project_module_graph import PlanHTML

    parsed = PlanHTML(document)
    section = parsed.section('dependencies', 'Dependencies')
    facts = parsed.section('facts', 'Plan facts')
    assert parsed.elements.index(section) < parsed.elements.index(facts)
    graph = plan['dependency_graph']
    incomplete = 'Incomplete plan. Showing resolved dependencies so far.'
    assert (incomplete in section.visible()) == (not graph['complete']), section.visible()
    if not graph['edges']:
        message = 'No dependencies.' if graph['complete'] else 'No resolved dependencies yet.'
        assert message in section.visible(), section.visible()
        return

    table, = (node for node in section.descendants if node.tag == 'table')
    assert [node.visible().strip() for node in table.descendants if node.tag == 'th'] == [
        'Consumer', 'Usage', 'Producer', 'Declared at',
    ]
    rows = [[node for node in row.descendants if node.tag == 'td']
            for row in table.descendants if row.tag == 'tr']
    rows = [row for row in rows if row]
    assert len(rows) == len(graph['edges']), rows
    manifest = parsed.section('manifest', 'build.ycxx')
    anchors = {node.attrs.get('id'): node for node in manifest.descendants}
    lines = plan['definition'].splitlines()
    for row, edge in zip(rows, graph['edges']):
        consumer, usage, producer, declared = row
        for cell, field, label, optional in (
            (consumer, 'consumer', None, None),
            (usage, 'usage', 'Binding', 'binding'),
            (producer, 'producer', 'Output', 'output'),
        ):
            wanted = [edge[field]]
            if optional in edge:
                wanted.append(edge[optional])
                assert label in cell.visible(), (edge, cell.visible())
            assert [node.visible() for node in cell.descendants if node.tag == 'code'] == wanted, (
                edge, cell.visible())
        line, column = edge['location']['line'], edge['location']['column']
        link, = (node for node in declared.descendants if node.tag == 'a')
        identifier = f'manifest-line-{line}'
        assert link.attrs['href'] == f'#{identifier}' and link.visible() == f'build.ycxx:{line}:{column}', edge
        assert anchors[identifier].visible() == lines[line - 1], edge
    assert not any(node.attrs.get('data-dependency-probe') == '1' for node in parsed.elements)


def scope_state(scope):
    state = {}
    for path in scope.rglob('*'):
        stat = path.lstat()
        state[path.relative_to(scope)] = stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_mode
    return state


def check_catalog_html(document, definition, targets):
    # Module discovery also imports the plan fact checks below.
    from project_module_graph import PlanHTML

    parsed = PlanHTML(document)
    assert any(node.tag in ('h1', 'h2') and node.visible().strip() == 'Workspace targets'
               for node in parsed.elements), document
    manifest = parsed.section('manifest', 'build.ycxx')
    if not targets:
        assert 'No target declarations.' in html.unescape(document), document
        return
    table, = (node for node in parsed.elements if node.tag == 'table')
    assert [node.visible().strip() for node in table.descendants if node.tag == 'th'] == [
        'Target', 'Rule', 'Declared at',
    ]
    rows = [[node for node in row.descendants if node.tag == 'td']
            for row in table.descendants if row.tag == 'tr']
    rows = [row for row in rows if row]
    assert len(rows) == len(targets), rows
    anchors = {node.attrs.get('id'): node for node in manifest.descendants}
    lines = definition.splitlines()
    for row, target in zip(rows, targets):
        line, column = target['location']['line'], target['location']['column']
        location = f'build.ycxx:{line}:{column}'
        assert [cell.visible().strip() for cell in row] == [target['name'], target['kind'], location], target
        link, = (node for node in row[-1].descendants if node.tag == 'a')
        assert link.attrs['href'] == f'#manifest-line-{line}' and link.visible().strip() == location, target
        assert anchors[f'manifest-line-{line}'].visible() == lines[line - 1], target
    assert not any(node.attrs.get('data-catalog-probe') == '1' for node in parsed.elements)


def check_catalog(ycxx, scope, env, definition, declarations, *, warm=False):
    before = scope_state(scope)
    daemon = daemon_identity(ycxx, scope, env)
    assert (daemon is not None) == warm, daemon

    def catalog(*args):
        result = run_client(ycxx, scope, env, 'show', '--targets', *args)
        assert result.returncode == 0 and not result.stderr, (args, result)
        return result.stdout

    obj = json.loads(catalog('--format=json'))
    assert obj.keys() == {'schema', 'targets'} and obj['schema'] == 1, obj
    targets = obj['targets']
    assert [(target['name'], target['kind']) for target in targets] == declarations, targets
    for target in targets:
        assert target.keys() == {'name', 'kind', 'location'}, target
        location = target['location']
        assert location.keys() == {'path', 'line', 'column'} and location['path'] == 'build.ycxx', location
        spelling = f"{target['kind']} {json.dumps(target['name'], ensure_ascii=False)}"
        check_location(location, definition, spelling)

    text = catalog()
    assert catalog('--format=text') == text
    heading, *rows = text.splitlines()
    assert heading == 'Workspace targets', text
    if targets:
        assert len(rows) == len(targets), text
        decoder = json.JSONDecoder()
        for row, target in zip(rows, targets):
            location = target['location']
            site = f"{location['path']}:{location['line']}:{location['column']}"
            row = row.strip()
            name, end = decoder.raw_decode(row)
            assert name == target['name'] and row[end:].split() == [target['kind'], site], row
    else:
        assert [row.strip() for row in rows] == ['No target declarations.'], text

    check_catalog_html(catalog('--format=html'), definition, targets)
    assert scope_state(scope) == before, 'catalog changed the workspace or build state'
    assert daemon_identity(ycxx, scope, env) == daemon, 'catalog changed the resident daemon'
    return obj


def check_catalog_options(ycxx, scope, env):
    before = scope_state(scope)
    for args in (
        ('--targets', 'app'), ('app', '--targets'),
        ('--targets', '--target', 'x86_64-pc-linux-gnu'),
        ('--target=x86_64-pc-linux-gnu', '--targets'),
        ('--targets', '--against', 'unused.json'),
        ('--format=source', '--targets'),
        ('--targets', '--format=source', '--format=html'),
        ('--targets', '--format=json', '--format=source'),
        ('--targets', '--targets'),
    ):
        result = run_client(ycxx, scope, env, 'show', *args)
        assert result.returncode == 2 and result.stderr and not result.stdout, (args, result)
    assert scope_state(scope) == before, 'invalid catalog flags changed the workspace'


def check_catalog_scopes(ycxx, scope, env):
    scope.mkdir()
    child = scope / 'child'
    recipe = scope / 'rules'
    vacant = scope / 'empty'
    broken = scope / 'broken'
    missing = scope / 'missing'
    for directory in (child, recipe, vacant, broken, missing):
        directory.mkdir()

    parent = f'''# {MARKUP}\r
use plugin("rules/package") as media
    media.cook {json.dumps(UNSAFE_TARGET)} {{}}
cpp.executable "leaf" {{ sources = ["main.cc"] }}
  cpp.library "é-last" {{ sources = ["other.cc"] }}
cpp.library "Z-first" {{ sources = ["other.cc"] }}
  cpp.library "group/leaf" {{ sources = ["other.cc"] }}'''
    current = '   generate "leaf" {}\ncpp.library "child-only" {}\n'
    empty = '# A plugin import is not a declared target.\nuse plugin("absent/package") as ghost\n'
    (scope / 'build.ycxx').write_bytes(parent.encode('utf-8'))
    (child / 'build.ycxx').write_text(current, encoding='utf-8')
    (vacant / 'build.ycxx').write_text(empty, encoding='utf-8')
    (broken / 'build.ycxx').write_text('cpp.library "dupe" {}\ncpp.executable "dupe" {}\n', encoding='utf-8')

    script = 'printf ran > "$1"; exit 7'
    (recipe / 'build.ycxx').write_text(f'''host_action "package" {{
    tool = "/bin/sh"
    args = ["-c", {json.dumps(script)}, "recipe", {json.dumps(str(scope / 'recipe-ran'))}, output("plugin")]
    outputs = {{ plugin = "rules.so" }}
}}
''', encoding='utf-8')

    before = scope_state(scope)
    check_catalog(ycxx, scope, env, parent, [
        ('Z-first', 'cpp.library'), (UNSAFE_TARGET, 'media.cook'), ('group/leaf', 'cpp.library'),
        ('leaf', 'cpp.executable'), ('é-last', 'cpp.library'),
    ])
    check_catalog(ycxx, child, env, current, [('child-only', 'cpp.library'), ('leaf', 'generate')])
    check_catalog(ycxx, vacant, env, empty, [])
    for directory in (broken, missing):
        result = run_client(ycxx, directory, env, 'show', '--targets', '--format=json')
        assert result.returncode == 1 and 'build.ycxx' in result.stderr and not result.stdout, result
        if directory == broken:
            assert 'duplicate target' in result.stderr, result
    assert scope_state(scope) == before, 'catalog touched a nested workspace or package recipe'


def check_catalog_rejection(ycxx, scope, env, diagnostic, *, format_='json', line=None):
    before = scope_state(scope)
    runtime = Path(env['YCXX_RUNTIME_DIR'])
    runtime_before = scope_state(runtime)
    try:
        result = subprocess.run([ycxx, 'show', '--targets', f'--format={format_}'],
                                cwd=scope, env=env, capture_output=True, timeout=20)
    except subprocess.TimeoutExpired as error:
        raise AssertionError(f'catalog blocked reading {scope / "build.ycxx"}') from error
    assert result.returncode == 1 and not result.stdout, result
    stderr = result.stderr.decode('utf-8')
    location = 'build.ycxx' if line is None else f'build.ycxx:{line}:'
    assert diagnostic in stderr and location in stderr, stderr
    assert scope_state(scope) == before and scope_state(runtime) == runtime_before, 'rejected catalog wrote state'


def check_catalog_manifest_paths(ycxx, scope, env):
    for name in ('fifo', 'linked-fifo', 'directory', 'linked-file'):
        (scope / name).mkdir(parents=True)
    fifo = scope / 'fifo' / 'build.ycxx'
    os.mkfifo(fifo, 0o600)
    (scope / 'linked-fifo' / 'build.ycxx').symlink_to(fifo)
    (scope / 'directory' / 'build.ycxx').mkdir()
    for name in ('fifo', 'linked-fifo', 'directory'):
        check_catalog_rejection(ycxx, scope / name, env, 'not a regular file')

    linked = scope / 'linked-file'
    definition = 'cpp.library "é" {}\n  cpp.library "库/🚀" {}\n'
    (linked / 'definition.ycxx').write_text(definition, encoding='utf-8')
    (linked / 'build.ycxx').symlink_to('definition.ycxx')
    check_catalog(ycxx, linked, env, definition, [('é', 'cpp.library'), ('库/🚀', 'cpp.library')])


def check_catalog_manifest_encoding(ycxx, scope, env):
    scope.mkdir(parents=True)
    manifest = scope / 'build.ycxx'
    for definition, line in (
        (b'# target names\ncpp.library "caf\xe9" {}\n', 2),
        (b'# bad comment \xe9\ncpp.library "valid" {}\n', 1),
        (b'cpp.library "overlong \xc0\xaf" {}\n', 1),
        (b'cpp.library "surrogate \xed\xa0\x80" {}\n', 1),
        (b'cpp.library "out of range \xf4\x90\x80\x80" {}\n', 1),
    ):
        manifest.write_bytes(definition)
        for format_ in ('json', 'text', 'html'):
            check_catalog_rejection(ycxx, scope, env, 'invalid UTF-8', format_=format_, line=line)


def check_unresolved(ycxx, scope, definition):
    assert show(ycxx, scope, '--format=source') == definition.encode('utf-8')
    plan = show_json(ycxx, scope)
    assert plan['definition'] == definition, plan
    unresolved = fact(plan, 'unresolved', 'app', 'standard/rules')
    assert unresolved['name'] == 'cpp.executable', unresolved
    check_location(unresolved, definition, 'cpp.executable "app"')
    assert not any(item['kind'] == 'source' for item in plan['facts']), plan
    check_dependency_graph(plan, [], complete=False)
    check_dependency_html(show(ycxx, scope, '--format=html').decode('utf-8'), plan)
    assert not (scope / '.ycxx').exists(), 'cold inspection created build state'
    assert not (scope / 'generator-ran').exists(), 'cold inspection ran the generator'


def check_views(ycxx, scope, definition):
    before = scope_state(scope)
    assert show(ycxx, scope, '--format=source') == definition.encode('utf-8')
    snapshot = show_json(ycxx, scope)
    assert show_json(ycxx, scope) == snapshot, 'same plan produced different inspection output'
    assert snapshot['definition'] == definition, snapshot
    assert not any(item['kind'] == 'unresolved' for item in snapshot['facts']), snapshot

    literal = fact(snapshot, 'source', 'app', 'src/main.cc')
    check_location(literal, definition, '"src/main.cc"')
    expanded = fact(snapshot, 'source', 'app', 'auto/first.cc')
    assert 'glob("auto/*.cc")' in expanded['origin'], expanded
    check_location(expanded, definition, 'glob("auto/*.cc")')
    generated = fact(snapshot, 'source', 'app', '.ycxx/virtual/wire/wire.cc')
    assert 'wire' in generated['origin'] and 'source' in generated['origin'], generated
    check_location(generated, definition, 'artifact("wire", "source")')
    inherited = fact(snapshot, 'include', 'app', 'include')
    assert 'core' in inherited['origin'], inherited
    check_location(inherited, definition, '"include"')
    assert fact(snapshot, 'output', 'wire', '.ycxx/virtual/wire/wire.h')['name'] == 'header'
    fact(snapshot, 'tool', 'wire', 'generator.sh')
    fact(snapshot, 'input', 'wire', 'artifacts/schema/schema.txt')
    check_dependency_graph(snapshot, [
        dependency_edge(definition, 'app', 'wire', 'sources', 'artifact("wire", "source")', output='source'),
        dependency_edge(definition, 'app', 'wire', 'generated_headers', 'artifact("wire", "header")', output='header'),
        dependency_edge(definition, 'app', CORE, 'deps', json.dumps(CORE), start=definition.index('deps = [')),
        dependency_edge(definition, 'wire', 'wire_schema', 'inputs', 'artifact("wire_schema", "schema")',
                        output='schema', binding='schema'),
    ])

    text = show(ycxx, scope).decode('utf-8')
    assert show(ycxx, scope, '--format=text') == text.encode('utf-8')
    assert all(fragment in text for fragment in ('auto/first.cc', 'src/main.cc', 'wire.cc', 'core')), text

    document = show(ycxx, scope, '--format=html').decode('utf-8')
    visible = html.unescape(document)
    assert all(fragment in visible for fragment in (MARKUP, UNSAFE_SOURCE, 'auto/first.cc', 'src/main.cc')), document
    assert MARKUP not in document and UNSAFE_SOURCE not in document, 'inspection HTML contains unescaped user text'
    assert '&lt;script&gt;' in document and '&lt;b&gt;&amp;' in document, 'inspection HTML did not escape markup'
    check_dependency_html(document, snapshot)
    assert scope_state(scope) == before, 'inspection changed files or build state'

    assert not (scope / 'generator-ran').exists(), 'inspection ran the generator'
    assert not (scope / '.ycxx' / 'out').exists(), 'inspection published build output'
    return snapshot


def check_diff(ycxx, scope, definition, snapshot):
    before = scope / 'before.json'
    earlier_schema_1 = {key: value for key, value in snapshot.items() if key != 'dependency_graph'}
    before.write_text(json.dumps(earlier_schema_1), encoding='utf-8')
    (scope / 'auto' / 'new.cc').write_text('', encoding='utf-8')
    after = show_json(ycxx, scope)
    assert after['definition'] == snapshot['definition'] == definition, 'the glob changed without a manifest edit'

    diff = show_json(ycxx, scope, '--against', before.name)
    new_source = fact(after, 'source', 'app', 'auto/new.cc')
    changes = diff['changes']
    assert len(changes) == 1 and changes[0]['change'] == 'added', diff
    assert changes[0]['after'] == new_source and changes[0].get('before') is None, diff
    document = show(ycxx, scope, '--format=html', '--against', before.name).decode('utf-8')
    check_dependency_html(document, after)

    baseline = scope / 'after.json'
    baseline.write_text(json.dumps(after), encoding='utf-8')
    manifest = scope / 'build.ycxx'
    shifted = '# shift all origin locations\n\n' + definition
    manifest.write_bytes(shifted.encode('utf-8'))
    moved = show_json(ycxx, scope)
    assert fact(moved, 'source', 'app', 'auto/new.cc')['line'] == new_source['line'] + 2
    assert show_json(ycxx, scope, '--against', baseline.name)['changes'] == [], 'provenance alone changed the plan'

    (scope / 'auto' / 'new.cc').unlink()
    removed = show_json(ycxx, scope, '--against', baseline.name)['changes']
    assert len(removed) == 1 and removed[0]['change'] == 'removed', removed
    assert removed[0]['before'] == new_source and removed[0].get('after') is None, removed


def main():
    ycxx = Path(sys.argv[1]).resolve()
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-inspection-') as directory,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        scope = Path(directory).resolve()
        definition = prepare(scope)
        (scope / 'build.ycxx').write_bytes(definition.encode('utf-8'))
        env = env_for(runtime)
        cold_catalog = check_catalog(ycxx, scope, env, definition, INSPECTION_TARGETS)
        check_catalog_options(ycxx, scope, env)
        check_catalog_scopes(ycxx, scope / 'catalog-nesting', env)
        check_catalog_manifest_paths(ycxx, scope / 'catalog-paths', env)
        check_catalog_manifest_encoding(ycxx, scope / 'catalog-encoding', env)
        check_unresolved(ycxx, scope, definition)
        (scope / '.ycxx').mkdir()
        try:
            traced_build(ycxx, scope, env, 'admit the inspection plan')
            assert check_catalog(ycxx, scope, env, definition, INSPECTION_TARGETS, warm=True) == cold_catalog
            marker = scope / 'generator-ran'
            assert marker.read_text(encoding='utf-8') == 'ran', 'build did not run the generator'
            marker.unlink()
            out = scope / '.ycxx' / 'out'
            (out / 'app').unlink()
            out.rmdir()

            # Keep the admitted rule plans while making an accidental execution observable.
            generator = scope / 'generator.sh'
            generator.write_text('#!/bin/sh\nprintf ran > "$1"\nexit 7\n', encoding='utf-8')
            snapshot = check_views(ycxx, scope, definition)

            # Plan inspection still works with unavailable execution inputs.
            generator.unlink()
            (scope / 'schema.txt').unlink()
            assert show_json(ycxx, scope) == snapshot, 'inspection depended on action inputs being available'
            check_diff(ycxx, scope, definition, snapshot)
            assert not marker.exists(), 'inspection ran the generator'
            assert not out.exists(), 'inspection restored a removed build output'
        finally:
            run_client(ycxx, scope, env, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
