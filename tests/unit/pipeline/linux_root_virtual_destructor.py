"""Link ycxx's root virtual destructor to a native key function and its ELF table."""

import os
from pathlib import Path
import shutil
import sys
import tempfile

from linux_base_object_variants import native_compilers
from linux_compile import run


# Itanium C++ ABI [mangle.ctor-dtor-name] and sections 5.2.3 through 5.2.5.
DELETING = '_ZN1RD0Ev'
COMPLETE = '_ZN1RD1Ev'
BASE = '_ZN1RD2Ev'
DESTRUCTORS = {DELETING, COMPLETE, BASE}
KEY = '_ZN1R3keyEv'
VTABLE = '_ZTV1R'
ROOT_DATA = {VTABLE, '_ZTI1R', '_ZTS1R'}
# The Linux slice selects the sized form where [expr.delete] permits either form.
SIZED_DELETE = '_ZdlPvm'
UNSIZED_DELETE = '_ZdlPv'
ROOT_SYMBOLS = DESTRUCTORS | ROOT_DATA | {KEY, SIZED_DELETE, UNSIZED_DELETE}


def check_symbols(readelf, scope, env, obj, functions, objects, references):
    metadata = run([readelf, '-Ws', obj], scope, env)
    columns = (line.split() for line in metadata.splitlines())
    rows = [row for row in columns if len(row) == 8 and row[0].endswith(':')
            and row[0][:-1].isdigit() and row[7] in ROOT_SYMBOLS]
    by_name = {row[7]: row for row in rows}
    assert len(by_name) == len(rows) and by_name.keys() == functions | objects | references, metadata

    for name in functions | objects:
        _, _, size, kind, binding, visibility, section, _ = by_name[name]
        assert int(size) > 0 and visibility == 'DEFAULT' and section.isdigit(), metadata
        if name in functions:
            assert kind == 'FUNC' and binding == 'GLOBAL', metadata
        else:
            # GCC uses weak root metadata where Clang uses global definitions.
            assert kind == 'OBJECT' and binding in ('GLOBAL', 'WEAK'), metadata
    for name in references:
        assert by_name[name][4:7] == ['GLOBAL', 'DEFAULT', 'UND'], metadata
    if DELETING in functions:
        locations = {name: (by_name[name][6], by_name[name][1]) for name in DESTRUCTORS}
        assert locations[DELETING] not in (locations[COMPLETE], locations[BASE]), metadata


def main():
    ycxx, host, readelf = sys.argv[1:]
    fixture = Path(__file__).resolve().with_suffix('')
    env = {key: value for key, value in os.environ.items() if key not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='2')
    with tempfile.TemporaryDirectory(prefix='ycxx-root-virtual-dtor-') as temp:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        for source in ('root.hpp', 'producer.cpp'):
            shutil.copyfile(fixture / source, scope / source)
        run([ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-c', 'producer.cpp', '-o', 'producer.o'],
            scope, env)
        check_symbols(readelf, scope, env, 'producer.o', DESTRUCTORS, set(), {VTABLE, SIZED_DELETE})

        for compiler in native_compilers(host, scope, env):
            flags = [compiler, '-std=c++20', '-O0', '-pedantic-errors', '-Wall', '-Wextra', '-Werror',
                     '-fno-lto', '-fno-pie', '-fno-exceptions', '-fsized-deallocation']
            for unit in ('peer', 'dispatch'):
                run([*flags, '-c', fixture / f'{unit}.cpp', '-o', f'{unit}.o'], scope, env)
            check_symbols(readelf, scope, env, 'peer.o', {KEY, SIZED_DELETE, UNSIZED_DELETE},
                          ROOT_DATA, DESTRUCTORS)
            mixed = f'mixed-{Path(compiler).name}'
            run([compiler, '-fno-lto', '-no-pie', 'producer.o', 'peer.o', 'dispatch.o', '-o', mixed],
                scope, env)
            run([f'./{mixed}'], scope, env)


if __name__ == '__main__':
    main()
