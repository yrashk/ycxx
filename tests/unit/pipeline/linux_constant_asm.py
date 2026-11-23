"""Check constexpr namespace asm that marks existing ELF symbols hidden."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from linux_compile import run


ACCESSOR_SOURCE = '''\
#include "symbol.hpp"
struct accessor_directive {
  constexpr const char* data() const { return accessor_text(); }
  constexpr unsigned long size() const { return sizeof(".hidden accessor_first\\n") - 1; }
};
asm((accessor_directive{}));
extern "C" {
int accessor_first = 17;
int accessor_other = 19;
int accessor_neighbor = 23;
}
'''
ACCESSOR_HEADER = 'constexpr const char* accessor_text() {{ return ".hidden {target}\\n"; }}\n'
WHOLE_UNIT = 'struct Whole { virtual void f(); };\n'


def symbols(readelf, scope, env, neighbor_hidden):
    table = run([readelf, '-Ws', 'directive.o'], scope, env)
    expected = {
        'probe_value': ('GLOBAL', 'HIDDEN'),
        'neighbor_value': ('GLOBAL', 'HIDDEN' if neighbor_hidden else 'DEFAULT'),
        '_Z11weak_targetv': ('WEAK', 'DEFAULT' if neighbor_hidden else 'HIDDEN'),
        'imported_value': ('GLOBAL', 'HIDDEN'),
    }
    entries = {}
    for name, binding in expected.items():
        entry, = (line.split() for line in table.splitlines() if line.split()[-1:] == [name])
        assert tuple(entry[4:6]) == binding, (name, binding, entry)
        assert entry[6] == 'UND' if name == 'imported_value' else entry[6].isdecimal(), entry
        entries[name] = entry
    assert entries['probe_value'][2:4] == entries['neighbor_value'][2:4], entries
    assert entries['_Z11weak_targetv'][3] == 'FUNC', entries
    assert 'unexpected_suffix' not in table, table

    relocations = run([readelf, '-Wr', 'directive.o'], scope, env)
    references = [row for line in relocations.splitlines()
                  if len(row := line.split()) >= 5 and row[2].startswith('R_X86_64_') and row[4] == 'imported_value']
    assert len(references) == 1 and int(references[0][1], 16) >> 32 == int(entries['imported_value'][0][:-1]), relocations


def accessor_symbols(readelf, scope, env, target):
    table = run([readelf, '-Ws', 'directive.o'], scope, env)
    rows = [row for line in table.splitlines() if len(row := line.split()) >= 8]
    assert {row[7] for row in rows if row[5] == 'HIDDEN'} == {target}, table
    attributes = {}
    for name in ('accessor_first', 'accessor_other', 'accessor_neighbor'):
        row, = (row for row in rows if row[7] == name)
        assert row[4] == 'GLOBAL' and row[6].isdecimal(), row
        assert row[5] == ('HIDDEN' if name == target else 'DEFAULT'), row
        attributes[name] = row[2:5]
    return attributes


def replay(options, readelf, scope, env, cold, resident):
    header, obj = scope / 'symbol.hpp', scope / 'directive.o'
    ycxx = options[0]
    args = [*options, '-c', 'directive.cpp', '-o', obj.name]

    def build(context):
        obj.unlink(missing_ok=True)
        run(args, scope, context)
        return obj.read_bytes()

    def pid():
        status = run([ycxx, 'ctl', 'status'], scope, env)
        found, = (line for line in status.splitlines() if line.startswith('pid: '))
        return found

    snapshots = []
    for target in ('_Z11weak_targetv', 'neighbor_value', '_Z11weak_targetv'):
        header.write_text(f'#define BUFFERED_SYMBOL "{target}"\n')
        fresh = build(cold)
        assert build(resident) == fresh, target
        symbols(readelf, scope, env, target == 'neighbor_value')
        if not snapshots:
            initial_pid = pid()
        snapshots.append(fresh)
    assert snapshots[0] == snapshots[2] and snapshots[0] != snapshots[1]
    assert pid() == initial_pid

    # Only the header helper's body changes while each asm item stays byte-identical.
    source = scope / 'directive.cpp'
    first = 'accessor_first'
    attributes = []
    for trailer in ('', WHOLE_UNIT):
        source.write_text(ACCESSOR_SOURCE + trailer)
        snapshots = []
        for target in (first, 'accessor_other', first):
            header.write_text(ACCESSOR_HEADER.format(target=target))
            fresh = build(cold)
            assert build(resident) == fresh, (trailer, target)
            snapshots.append(fresh)
            attributes.append(accessor_symbols(readelf, scope, env, target))
        assert snapshots[0] == snapshots[2] and snapshots[0] != snapshots[1], trailer
        if not trailer:
            grouped_object = snapshots[0]
    assert all(entry == attributes[0] for entry in attributes), attributes

    source.write_text(ACCESSOR_SOURCE)
    assert build(cold) == build(resident) == grouped_object
    assert accessor_symbols(readelf, scope, env, first) == attributes[0]
    assert pid() == initial_pid


def rejected(options, scope, cold):
    data = 'constexpr const char* data() const { return ".hidden probe_value"; }'
    size = 'constexpr unsigned long size() const { return sizeof(".hidden probe_value") - 1; }'
    cases = {
        'missing-size': data,
        'wrong-data-type': 'constexpr int data() const { return 0; } ' + size,
        'nonconstant-data': data.removeprefix('constexpr ') + ' ' + size,
        'out-of-range': data + ' ' + size.replace('- 1', '+ 1'),
    }
    for name, literal in (('extra-directive', r'.hidden probe_value\n.globl accidental'),
                          ('embedded-nul', r'.hidden probe_value\0suffix')):
        cases[name] = data.replace('.hidden probe_value', literal) + ' ' + size.replace('.hidden probe_value', literal)

    output = scope / 'rejected.o'
    for name, accessors in cases.items():
        source = scope / f'{name}.cpp'
        source.write_text(f'struct directive {{ {accessors} }};\n'
                          'asm((directive{}));\nextern "C" { int probe_value = 23; }\n')
        output.unlink(missing_ok=True)
        result = subprocess.run([*options, '-c', source.name, '-o', output.name], cwd=scope, env=cold,
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 1 and ': error:' in result.stderr and not output.exists(), (name, result)


def main():
    ycxx, readelf = sys.argv[1:]
    options = (ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26')
    source = Path(__file__).resolve().with_suffix('.cpp')
    with tempfile.TemporaryDirectory(prefix='ycxx-constant-asm-') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-', dir='/tmp') as runtime:
        scope = Path(temp).resolve()
        (scope / '.ycxx').touch()
        (scope / 'directive.cpp').write_bytes(source.read_bytes())
        env = {key: value for key, value in os.environ.items()
               if key not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT', 'YCXX_NO_WATCH')}
        env.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2', XDG_CACHE_HOME=str(scope / 'cache'))
        cold = dict(env, YCXX_NO_DAEMON='1')
        resident = dict(env, YCXX_DAEMON='require')
        try:
            replay(options, readelf, scope, env, cold, resident)
            rejected(options, scope, cold)
        finally:
            subprocess.run([ycxx, 'ctl', 'shutdown'], cwd=scope, env=env, capture_output=True, timeout=30)


if __name__ == '__main__':
    main()
