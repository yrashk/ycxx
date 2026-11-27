"""Run tests/own/classes/virtual-dispatch on Linux x86-64 (#309).

The run suite builds and runs those tests on macOS. Here ycxx compiles each test to an ELF
object, and every native host compiler links it, with the test's PEER objects compiled by that
compiler, against its own C++ runtime. Each executable must exit with the test's EXIT status.
The objects' tables and RTTI must be defined objects; a weak one has an ELF COMDAT group of its
own (Itanium C++ ABI §5.2.3-4).
"""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from linux_base_object_variants import native_compilers
from linux_compile import run


def directives(text, key):
    return re.findall(rf'^// {key}: *(\S.*?)\s*$', text, re.M)


def check_tables(readelf, scope, env, obj):
    symbols = run([readelf, '-Ws', obj], scope, env)
    groups = run([readelf, '-g', obj], scope, env)
    group_names = set(re.findall(r'COMDAT group section \[\s*\d+\] `\.group\' \[([^\]]+)\]', groups))
    for line in symbols.splitlines():
        row = line.split()
        if len(row) != 8 or not row[0].endswith(':') or not row[7].startswith(('_ZTV', '_ZTI', '_ZTS')):
            continue
        _, _, size, kind, binding, visibility, section, name = row
        if section == 'UND':
            continue
        assert kind == 'OBJECT' and int(size) > 0 and visibility == 'DEFAULT', f'{obj}: {line}\n{symbols}'
        assert binding in ('GLOBAL', 'WEAK'), f'{obj}: {line}\n{symbols}'
        if binding == 'WEAK':
            assert name in group_names, f'{obj}: weak {name} has no COMDAT group\n{groups}'


def main():
    ycxx, host, readelf = sys.argv[1:]
    suite = Path(__file__).resolve().parents[2] / 'own' / 'classes' / 'virtual-dispatch'
    tests = sorted(suite.glob('*.run.cpp'))
    assert tests, suite
    env = {key: value for key, value in os.environ.items() if key not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='2')
    with tempfile.TemporaryDirectory(prefix='ycxx-virtual-dispatch-') as temp:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        compilers = native_compilers(host, scope, env)
        # A standalone key definition owns the table without any constructor installing it.
        producer = scope / 'keyed-producer.cpp'
        producer.write_text('struct Base { virtual int first() { return 1; } virtual int key(); };\n'
                            'int Base::key() { return 10; }\n')
        obj = scope / 'keyed-producer.o'
        run([ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26', '-fno-exceptions', '-c', producer, '-o', obj], scope, env)
        symbols = run([readelf, '-Ws', obj], scope, env)
        for name in ('_ZTV4Base', '_ZTI4Base'):
            assert re.search(rf'\bOBJECT\s+GLOBAL\s+DEFAULT\s+\d+\s+{name}$', symbols, re.M), symbols
        check_tables(readelf, scope, env, obj)
        # A late redeclaration can invalidate an earlier group's constructor layout.
        uncertain = scope / 'late-alignment.cpp'
        uncertain.write_text('struct A { virtual int f() { return 3; } int x; };\n'
                             'int use() { A a; return a.f(); }\n'
                             'struct __attribute__((aligned(32))) A;\n')
        result = subprocess.run([ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26', '-fno-exceptions',
                                 '-c', uncertain, '-o', scope / 'late-alignment.o'],
                                cwd=scope, env=env, capture_output=True, text=True, timeout=30)
        assert result.returncode and 'native output for a translation unit with virtual member functions' in result.stderr, result
        for test in tests:
            text = test.read_text()
            expected = int((directives(text, 'EXIT') or ['0'])[0])
            obj = scope / f'{test.name}.o'
            run([ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26', '-fno-exceptions', '-c', test, '-o', obj], scope, env)
            check_tables(readelf, scope, env, obj)
            for compiler in compilers:
                tag = Path(compiler).name
                peers = []
                for peer in directives(text, 'PEER'):
                    peer_obj = scope / f'{peer}.{tag}.o'
                    run([compiler, '-std=c++26', '-O0', '-fno-pie', '-fno-exceptions', '-c', suite / peer, '-o', peer_obj], scope, env)
                    peers.append(peer_obj)
                exe = scope / f'{test.name}.{tag}'
                run([compiler, '-no-pie', obj, *peers, '-o', exe], scope, env)
                result = subprocess.run([exe], cwd=scope, env=env, capture_output=True, text=True, timeout=30)
                assert result.returncode == expected, \
                    f'{test.name} linked by {tag} exited {result.returncode}, expected {expected}\n{result.stderr}'


if __name__ == '__main__':
    main()
