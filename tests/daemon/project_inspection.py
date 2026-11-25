"""Inspect source, resolved provenance, and semantic build diffs (#1256)."""

import html
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from project_build import env_for, run_client, traced_build
from runtime_dir import runtime_root


MARKUP = '<script>alert("definition")</script> & report'
UNSAFE_SOURCE = 'src/literal <b>&".cc'


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
generate "wire" {{
    tool = {json.dumps(str(generator))}
    args = [{json.dumps(str(scope / 'generator-ran'))}, input("schema"), output("source"), output("header")]
    inputs = {{ schema = "schema.txt" }}
    outputs = {{ source = "wire.cc" header = "wire.h" }}
}}
cpp.library "core" {{
    sources = ["src/core.cc"]
    public_include_dirs = ["include", "fallback"]
}}
cpp.executable "app" {{
    sources = ["src/main.cc", {json.dumps(UNSAFE_SOURCE)}, glob("auto/*.cc"), artifact("wire", "source")]
    generated_headers = [artifact("wire", "header")]
    deps = ["core"]
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


def check_location(item, definition, spelling):
    start = definition.index(spelling)
    line = definition.count('\n', 0, start) + 1
    column = start - definition.rfind('\n', 0, start)
    assert (item['line'], item['column']) == (line, column), (spelling, line, column, item)


def check_unresolved(ycxx, scope, definition):
    assert show(ycxx, scope, '--format=source') == definition.encode('utf-8')
    plan = show_json(ycxx, scope)
    assert plan['definition'] == definition, plan
    unresolved = fact(plan, 'unresolved', 'app', 'standard/rules')
    assert unresolved['name'] == 'cpp.executable', unresolved
    check_location(unresolved, definition, 'cpp.executable "app"')
    assert not any(item['kind'] == 'source' for item in plan['facts']), plan
    assert not (scope / '.ycxx').exists(), 'cold inspection created build state'
    assert not (scope / 'generator-ran').exists(), 'cold inspection ran the generator'


def check_views(ycxx, scope, definition):
    assert show(ycxx, scope, '--format=source') == definition.encode('utf-8')
    snapshot = show_json(ycxx, scope)
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
    fact(snapshot, 'input', 'wire', 'schema.txt')

    text = show(ycxx, scope).decode('utf-8')
    assert show(ycxx, scope, '--format=text') == text.encode('utf-8')
    assert all(fragment in text for fragment in ('auto/first.cc', 'src/main.cc', 'wire.cc', 'core')), text

    document = show(ycxx, scope, '--format=html').decode('utf-8')
    visible = html.unescape(document)
    assert all(fragment in visible for fragment in (MARKUP, UNSAFE_SOURCE, 'auto/first.cc', 'src/main.cc')), document
    assert MARKUP not in document and UNSAFE_SOURCE not in document, 'inspection HTML contains unescaped user text'
    assert '&lt;script&gt;' in document and '&lt;b&gt;&amp;' in document, 'inspection HTML did not escape markup'

    assert not (scope / 'generator-ran').exists(), 'inspection ran the generator'
    assert not (scope / '.ycxx' / 'out').exists(), 'inspection published build output'
    return snapshot


def check_diff(ycxx, scope, definition, snapshot):
    before = scope / 'before.json'
    before.write_text(json.dumps(snapshot), encoding='utf-8')
    (scope / 'auto' / 'new.cc').write_text('', encoding='utf-8')
    after = show_json(ycxx, scope)
    assert after['definition'] == snapshot['definition'] == definition, 'the glob changed without a manifest edit'

    diff = show_json(ycxx, scope, '--against', before.name)
    new_source = fact(after, 'source', 'app', 'auto/new.cc')
    changes = diff['changes']
    assert len(changes) == 1 and changes[0]['change'] == 'added', diff
    assert changes[0]['after'] == new_source and changes[0].get('before') is None, diff

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
        check_unresolved(ycxx, scope, definition)
        (scope / '.ycxx').mkdir()
        env = env_for(runtime)
        try:
            traced_build(ycxx, scope, env, 'admit the inspection plan')
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
