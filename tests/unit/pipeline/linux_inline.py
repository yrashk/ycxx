"""Resolve external inline functions and variables across x86-64 ELF objects."""

import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


def main():
    ycxx, host_cxx, readelf, ar = sys.argv[1:]
    fixtures = Path(__file__).resolve().parent
    fixture = fixtures / 'inline-odr'
    target = '--target=x86_64-pc-linux-gnu'
    env = dict(os.environ, LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='1')
    with tempfile.TemporaryDirectory(prefix='ycxx-inline-', dir='.') as temp:
        def run(*args, status=0, error=''):
            result = subprocess.run(args, cwd=temp, env=env, capture_output=True, text=True, timeout=30)
            if result.returncode != status or error not in result.stderr:
                raise AssertionError(f'{args} exited {result.returncode}, expected {status}'
                                     f' and stderr containing {error!r}\n{result.stdout}{result.stderr}')
            return result.stdout

        def compile(source, obj):
            run(ycxx, 'cxx', target, '-c', source, '-o', obj)

        def link(*inputs, status=0, error=''):
            run(ycxx, 'cxx', target, '-nostdlib', *inputs, '-o', 'program', status=status, error=error)

        def symbols(obj, *options):
            return {row[-1]: row for line in run(readelf, '-Ws', *options, obj).splitlines()
                    if len(row := line.split(maxsplit=7)) == 8 and row[0].endswith(':')}

        hidden = fixtures / 'inline-hidden'
        inline_hidden = {'hidden_step(int)': ('WEAK', 'HIDDEN', True),
                         'hidden_twice(int)': ('WEAK', 'HIDDEN', True)}
        for part, expected in {
            'main': inline_hidden,
            'peer': inline_hidden,
            'boundaries': {
                'hidden_from_decl(int)': ('GLOBAL', 'HIDDEN', True),
                'hidden_from_def(int)': ('GLOBAL', 'HIDDEN', True),
                'strong_hidden(int)': ('GLOBAL', 'HIDDEN', True),
                'inline_default(int)': ('WEAK', 'DEFAULT', True),
                'static_local(int)': ('LOCAL', 'DEFAULT', True),
                'overload(int)': ('GLOBAL', 'HIDDEN', True),
                'overload(long)': ('GLOBAL', 'DEFAULT', True),
                'unrelated_attr(int)': ('GLOBAL', 'DEFAULT', True),
            },
            'boundaries_main': {
                'hidden_from_decl(int)': ('GLOBAL', 'HIDDEN', False),
                'hidden_from_def(int)': ('GLOBAL', 'DEFAULT', False),
            },
            'declarator_visibility.accept': {
                'one(int)': ('GLOBAL', 'HIDDEN', False),
                'two(int)': ('GLOBAL', 'DEFAULT', False),
            },
        }.items():
            obj = f'hidden-{part}.o'
            compile(hidden / f'{part}.cpp', obj)
            table = symbols(obj, '--demangle')
            for name, binding in expected.items():
                entry = table.get(name)
                actual = (entry[4], entry[5], entry[6] != 'UND') if entry else None
                assert actual == binding, (obj, name, binding, entry)

        for objects in (('hidden-main.o', 'hidden-peer.o'), ('hidden-boundaries.o', 'hidden-boundaries_main.o')):
            for order in (objects, objects[::-1]):
                link(*order)
                run('./program')
                run(host_cxx, '-no-pie', *order, '-o', 'host-program')
                run('./host-program')

        inline_symbols = ('_Z12inline_firsti', '_Z12inline_lateri', '_ZN9Operation6memberEi',
                          '_ZN14DeclaredMember6memberEi', '_Z15implicit_inlinei')
        for name in ('main', 'peer'):
            obj = f'{name}.o'
            compile(fixture / f'{name}.cpp', obj)
            table = symbols(obj)
            for symbol in inline_symbols:
                assert table[symbol][4] == 'WEAK' and table[symbol][6] != 'UND', table[symbol]
            ordinary = table[f'_Z17{name}_direct_callsi']
            assert ordinary[4] == 'GLOBAL' and ordinary[6] != 'UND', ordinary
            relocations = run(readelf, '-Wr', obj).splitlines()
            for symbol in inline_symbols:
                if symbol == '_ZN14DeclaredMember6memberEi':
                    continue
                for kind in ('R_X86_64_64', 'R_X86_64_PLT32'):
                    assert any(kind in line.split() and symbol in line.split() for line in relocations), (symbol, kind)

        for objects in (('main.o', 'peer.o'), ('peer.o', 'main.o')):
            link(*objects)
            run('./program', 'one', 'two')
        run(ar, 'crs', 'libinline.a', 'peer.o')
        link('main.o', 'libinline.a')
        run('./program', 'one', 'two')

        # The compiler configured by CMake supplies a second producer and drives the system link.
        run(host_cxx, '-std=c++20', '-O0', '-fno-pie', '-c', fixture / 'peer.cpp', '-o', 'host-peer.o')
        for objects in (('main.o', 'host-peer.o'), ('host-peer.o', 'main.o')):
            run(host_cxx, '-no-pie', *objects, '-o', 'host-program')
            run('./host-program', 'one', 'two')

        friend = fixtures / 'inline-friend'
        for name in ('main', 'peer'):
            obj = f'friend-{name}.o'
            compile(friend / f'{name}.cpp', obj)
            table = symbols(obj)
            for symbol in ('_ZN14inline_address4readERKNS_4ItemE', '_ZN14inline_addressplERKNS_4ItemEi'):
                entry = table[symbol]
                assert entry[4] == 'WEAK' and entry[6] != 'UND', entry
        for objects in (('friend-main.o', 'friend-peer.o'), ('friend-peer.o', 'friend-main.o')):
            link(*objects)
            run('./program')

        for name in ('strong1', 'strong2'):
            source = f'{name}.cpp'
            text = 'int ordinary(int value) { return value; }\n'
            if name == 'strong1':
                text += 'int main() { return ordinary(0); }\n'
            (Path(temp) / source).write_text(text)
            compile(source, f'{name}.o')
        link('strong1.o', 'strong2.o', status=1, error="duplicate symbol '_Z8ordinaryi'")

        data = fixtures / 'inline-data'
        data_sections = {'_ZN4data11initializedE': '.data', '_ZN4data16zero_initializedE': '.bss',
                         '_ZN4data9immutableE': '.rodata', '_ZN4data10redeclaredE': '.data',
                         '_ZN4data11specializedIiEE': '.data'}
        for name in ('main', 'peer'):
            obj = f'data-{name}.o'
            compile(data / f'{name}.cpp', obj)
            table = symbols(obj)
            headers = run(readelf, '-WS', obj)
            sections = dict(re.findall(r'\[\s*(\d+)\]\s+(\.[^\s]+)', headers))
            relocations = run(readelf, '-Wr', obj).splitlines()
            for symbol, section in data_sections.items():
                entry = table[symbol]
                assert entry[4] == 'WEAK' and sections.get(entry[6]) == section, entry
                assert any('R_X86_64_64' in line.split() and symbol in line.split() for line in relocations), symbol

            rodata = re.search(r'\]\s+\.rodata\s+PROGBITS\s+\w+\s+([0-9a-fA-F]+)', headers)
            assert rodata, headers
            contents = (Path(temp) / obj).read_bytes()
            for symbol, payload in {'_ZN6Record4seedE': struct.pack('<i', 42),
                                    '_ZN6Record10descriptorE': struct.pack('<ii', 23, 47)}.items():
                entry = table[symbol]
                assert entry[4] == 'WEAK' and sections.get(entry[6]) == '.rodata', entry
                # Ordinary data can omit ELF symbol size. Verify the stored bytes in either case.
                assert int(entry[2]) == len(payload) or (entry[3] == 'NOTYPE' and entry[2] == '0'), entry
                offset = int(rodata[1], 16) + int(entry[1], 16)
                assert contents[offset:offset + len(payload)] == payload, (obj, symbol)
            local = table['_ZN12_GLOBAL__N_111LocalRecord4seedE']
            assert local[4] == 'LOCAL' and sections.get(local[6]) == '.rodata', local

        for objects in (('data-main.o', 'data-peer.o'), ('data-peer.o', 'data-main.o')):
            link(*objects)
            run('./program')
        run(host_cxx, '-std=c++20', '-O0', '-fno-pie', '-c', data / 'peer.cpp', '-o', 'host-data-peer.o')
        for objects in (('data-main.o', 'host-data-peer.o'), ('host-data-peer.o', 'data-main.o')):
            run(host_cxx, '-no-pie', *objects, '-o', 'host-program')
            run('./host-program')

        assignment = fixtures / 'inline-assignment'
        for name in ('main', 'peer'):
            obj = f'assignment-{name}.o'
            compile(assignment / f'{name}.cpp', obj)
            entry = symbols(obj)['_ZN1HaSERKS_']
            assert entry[4] == 'WEAK' and entry[6] != 'UND', entry
        for objects in (('assignment-main.o', 'assignment-peer.o'), ('assignment-peer.o', 'assignment-main.o')):
            link(*objects)
            run('./program')

        (Path(temp) / 'data-controls.cpp').write_text('''
constexpr int ordinary_constant = 7;
extern constexpr int external_constant = 9;
static inline int local_inline = 11;
struct Ordinary { static int value; };
int Ordinary::value = 13;
int backing = 13;
inline int* relocated = &backing;
template<class T> inline int specialized = 17;
template<> int specialized<int> = 19;
struct NamedType {};
template<> inline int specialized<NamedType> = 23;
''')
        compile('data-controls.cpp', 'data-controls.o')
        table = symbols('data-controls.o')
        for name in ('ordinary_constant', 'local_inline'):
            assert table[name][4] == 'LOCAL', table[name]
        for name in ('external_constant', 'backing', 'relocated', '_ZN8Ordinary5valueE',
                     '_Z11specializedIiE', '_Z11specializedI9NamedTypeE'):
            assert table[name][4] == 'GLOBAL' and table[name][6] != 'UND', table[name]


if __name__ == '__main__':
    main()
