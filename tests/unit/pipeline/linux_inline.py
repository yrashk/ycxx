"""Resolve external inline functions and variables across x86-64 ELF objects."""

import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


def weak_runtime_providers(scope, libycxx, host_cxx, readelf, compile, link, symbols, run):
    def defined_function(obj, name, binding, visibility=None):
        entry = symbols(obj)[name]
        assert int(entry[2]) > 0 and entry[3:5] == ['FUNC', binding] and entry[6].isdecimal(), (obj, entry)
        if visibility is not None:
            assert entry[5] == visibility, (obj, entry, visibility)

    debugger = '_ZNSt4__y119is_debugger_presentEv'
    provider = libycxx / 'src/runtime/debugging/is_debugger_present.cpp'
    headers = ('-isystem', libycxx / 'include')
    compile(provider, 'debugger-static.o', *headers, '-DYCXX_SHARED=0')
    defined_function('debugger-static.o', debugger, 'WEAK')
    compile(provider, 'debugger-shared.o', *headers, '-DYCXX_SHARED=1')
    defined_function('debugger-shared.o', debugger, 'WEAK', 'DEFAULT')

    # Use the pinned freestanding PAL fallback and static linkage marker for the real default.
    compile(libycxx / 'src/freestanding/pal/debugger_present.cpp', 'debugger-pal.o', *headers, '-DYCXX_SHARED=0')
    compile(libycxx / 'src/runtime/linkage/static.cpp', 'debugger-marker.o', *headers, '-DYCXX_SHARED=0')
    (scope / 'debugger-main.cpp').write_text(
        '#include <debugging>\nint main() { return std::is_debugger_present() ? 1 : 0; }\n')
    compile('debugger-main.cpp', 'debugger-main.o', *headers, '-DYCXX_SHARED=0')
    link('debugger-main.o', 'debugger-static.o', 'debugger-pal.o', 'debugger-marker.o')
    run('./program')

    units = {
        'weak-main': 'int selected() noexcept;\nint main() { return selected(); }\n',
        'weak-provider': '[[__gnu__::__weak__]] int selected() noexcept { return 17; }\n',
        'weak-redecl': ('extern "C" __attribute__((weak, visibility("hidden"))) int hook() noexcept;\n'
                        'int hook() noexcept { return 17; }\n'),
        'strong-provider': 'int selected() noexcept { return 31; }\n',
        'duplicate-provider': 'int selected() noexcept { return 41; }\n',
    }
    for name, contents in units.items():
        source = f'{name}.cpp'
        (scope / source).write_text(contents)
        compile(source, f'{name}.o')
    defined_function('weak-provider.o', '_Z8selectedv', 'WEAK', 'DEFAULT')
    defined_function('weak-redecl.o', 'hook', 'WEAK', 'HIDDEN')
    defined_function('strong-provider.o', '_Z8selectedv', 'GLOBAL', 'DEFAULT')
    assert '_Z8selectedv' not in run(readelf, '-Wg', 'weak-provider.o')

    link('weak-main.o', 'weak-provider.o')
    run('./program', status=17)
    for order in (('weak-provider.o', 'strong-provider.o'), ('strong-provider.o', 'weak-provider.o')):
        link('weak-main.o', *order)
        run('./program', status=31)
        run(host_cxx, '-no-pie', 'weak-main.o', *order, '-o', 'host-program')
        run('./host-program', status=31)
    link('weak-main.o', 'strong-provider.o', 'duplicate-provider.o', status=1, error="duplicate symbol '_Z8selectedv'")


def main():
    ycxx, host_cxx, readelf, ar, libycxx_source = sys.argv[1:]
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

        def compile(source, obj, *flags):
            run(ycxx, 'cxx', target, *flags, '-c', source, '-o', obj)

        def link(*inputs, status=0, error=''):
            run(ycxx, 'cxx', target, '-nostdlib', *inputs, '-o', 'program', status=status, error=error)

        def symbols(obj, *options):
            return {row[-1]: row for line in run(readelf, '-Ws', *options, obj).splitlines()
                    if len(row := line.split(maxsplit=7)) == 8 and row[0].endswith(':')}

        addresses = fixtures / 'object-address-members'
        shared = '_ZN6config5ProbeIXadL_ZNS_6sharedEEEE4readEv'
        local = '_ZN6config5ProbeIXadL_ZL13private_valueEEE4readEv'
        for part in ('main', 'peer'):
            obj = f'object-address-{part}.o'
            compile(addresses / f'{part}.cpp', obj)
            table = symbols(obj)
            assert table[local][3:5] == ['FUNC', 'LOCAL'] and table[local][6] != 'UND', table[local]
            assert table[shared][4] == 'GLOBAL' and (table[shared][6] != 'UND') == (part == 'main'), table[shared]
            run(host_cxx, '-std=c++20', '-pedantic-errors', '-O0', '-fno-pie', '-c', addresses / f'{part}.cpp',
                '-o', f'host-object-address-{part}.o')
        for objects in (('object-address-main.o', 'object-address-peer.o'),
                        ('object-address-peer.o', 'object-address-main.o')):
            link(*objects)
            run('./program')
        for pairing in (('object-address-main.o', 'host-object-address-peer.o'),
                        ('host-object-address-main.o', 'object-address-peer.o')):
            for objects in (pairing, pairing[::-1]):
                run(host_cxx, '-no-pie', *objects, '-o', 'host-program')
                run('./host-program')

        weak_runtime_providers(Path(temp), Path(libycxx_source), host_cxx, readelf, compile, link, symbols, run)

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

        # A separate object refers to a private inline definition by its raw linker name.
        # A constant-evaluated call to a different helper in the provider leaves it available.
        (Path(temp) / 'raw-name-provider.cpp').write_text('''\
class Hidden {
    static constexpr int helper(int x) { return x + 9; }
    static constexpr int evaluated(int x) { return x + 1; }
public:
    static constexpr int choose(int x) {
        if consteval { return evaluated(x); } else { return x; }
    }
};
''')
        (Path(temp) / 'raw-name-consumer.cpp').write_text('''\
int demand(int) __asm__("_ZN6Hidden6helperEi");
int main() { return demand(33) - 42; }
''')
        compile('raw-name-provider.cpp', 'raw-name-provider.o')
        compile('raw-name-consumer.cpp', 'raw-name-consumer.o')
        entry = symbols('raw-name-provider.o').get('_ZN6Hidden6helperEi')
        assert entry and entry[3:5] == ['FUNC', 'WEAK'] and entry[6] != 'UND', entry
        for objects in (('raw-name-consumer.o', 'raw-name-provider.o'),
                        ('raw-name-provider.o', 'raw-name-consumer.o')):
            link(*objects)
            run('./program')

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
            for symbol in ('_ZN14inline_address4readERKNS_4ItemE', '_ZN14inline_addressplERKNS_4ItemEi',
                           '_ZN15inline_template4takeERKNS_4CellIiEE'):
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

        constructors = fixtures / 'inline-constructors'
        for name in ('main', 'peer'):
            compile(constructors / f'{name}.cpp', f'ctor-{name}.o')
        for objects in (('ctor-main.o', 'ctor-peer.o'), ('ctor-peer.o', 'ctor-main.o')):
            link(*objects)
            run('./program')
        strong_ctor = '_ZN9OutOfLineC1Ei'
        for name in ('main', 'peer'):
            table = symbols(f'ctor-{name}.o')
            for ctor, binding in (
                ('_ZN7InClassC1Ei', 'WEAK'), ('_ZN12OnDefinitionC1Ei', 'WEAK'),
                ('_ZN13OnDeclarationC1Ei', 'WEAK'), ('_ZN12_GLOBAL__N_19LocalCtorC1Ei', 'LOCAL'),
            ):
                entry = table[ctor]
                assert entry[4] == binding and entry[6] != 'UND', (name, entry)
            entry = table[strong_ctor]
            assert entry[4] == 'GLOBAL' and (entry[6] != 'UND') == (name == 'main'), (name, entry)
        compile(constructors / 'peer.cpp', 'duplicate-ctor.o', '-DDUPLICATE_CONSTRUCTOR')
        for objects in (('ctor-main.o', 'duplicate-ctor.o'), ('duplicate-ctor.o', 'ctor-main.o')):
            link(*objects, status=1, error=f"duplicate symbol '{strong_ctor}'")

        destructors = fixtures / 'inline-destructors'
        for fixture, binding, names in (
            ('shared', 'WEAK', ('_ZN6MemberIiED1Ev', '_ZN6MemberIbED1Ev', '_ZN5OwnerD1Ev')),
            ('local', 'LOCAL', ('_ZN6MemberIN12_GLOBAL__N_13TagEED1Ev', '_ZN5OwnerIN12_GLOBAL__N_13TagEED1Ev')),
        ):
            first, second = f'{fixture}-dtor-first.o', f'{fixture}-dtor-second.o'
            compile(destructors / fixture / 'first.cpp', first)
            compile(destructors / fixture / 'second.cpp', second)
            for obj in (first, second):
                table = symbols(obj)
                for name in names:
                    assert table[name][4] == binding and table[name][6] != 'UND', (obj, table[name])
            for objects in ((first, second), (second, first)):
                link(*objects)
                run('./program')

        compile(destructors / 'enum-value.cpp', 'enum-dtor.o')
        table = symbols('enum-dtor.o')
        for name, binding in (('_ZN6MemberILN12_GLOBAL__N_13TagE7EED1Ev', 'LOCAL'),
                              ('_ZN12PublicMemberIL9PublicTag7EED1Ev', 'WEAK')):
            assert table[name][4] == binding and table[name][6] != 'UND', table[name]
        link('enum-dtor.o')
        run('./program')

        ordinary = destructors / 'out-of-line'
        compile(ordinary / 'first.cpp', 'strong-dtor-first.o')
        compile(ordinary / 'second.cpp', 'strong-dtor-second.o')
        entry = symbols('strong-dtor-first.o')['_ZN5OwnerD1Ev']
        assert entry[4] == 'GLOBAL' and entry[6] != 'UND', entry
        link('strong-dtor-first.o', 'strong-dtor-second.o')
        run('./program')
        compile(ordinary / 'second.cpp', 'duplicate-dtor.o', '-DDUPLICATE_DESTRUCTOR')
        link('strong-dtor-first.o', 'duplicate-dtor.o', status=1, error="duplicate symbol '_ZN5OwnerD1Ev'")

        specialized = destructors / 'specialized'
        symbol = '_ZN4CellIiED1Ev'
        for name, flags, binding in (
            ('written', ('-DWRITTEN_INLINE',), 'WEAK'),
            ('declared', ('-DDECLARED_INLINE',), 'WEAK'),
            ('strong', (), 'GLOBAL'),
        ):
            main_obj, peer_obj = f'{name}-specialized-main.o', f'{name}-specialized-peer.o'
            compile(specialized / 'main.cpp', main_obj, *flags)
            compile(specialized / 'peer.cpp', peer_obj, *flags)
            for obj in (main_obj, peer_obj):
                entry = symbols(obj)[symbol]
                assert entry[4] == binding and entry[6] != 'UND', (obj, entry)
            if binding == 'GLOBAL':
                link(main_obj, peer_obj, status=1, error=f"duplicate symbol '{symbol}'")
            else:
                for objects in ((main_obj, peer_obj), (peer_obj, main_obj)):
                    link(*objects)
                    run('./program')

        compile(data / 'bindings.cpp', 'data-controls.o')
        table = symbols('data-controls.o')
        entry = table['_ZN6Public6scalarE']
        assert entry[4] == 'WEAK' and entry[6] != 'UND', entry
        for name in ('ordinary_constant', 'local_inline', '_ZN6Public1pE', '_ZN6Public1kE'):
            assert table[name][4] == 'LOCAL', table[name]
        for name in ('external_constant', 'backing', 'relocated', '_ZN8Ordinary5valueE',
                     '_Z11specializedIiE', '_Z11specializedI9NamedTypeE'):
            assert table[name][4] == 'GLOBAL' and table[name][6] != 'UND', table[name]

        static_members = fixtures / 'inline-template-statics'
        for case, expected in (
            ('constant', {}),
            ('address', {'_ZN7ProfileILj2EE5limitE': 'WEAK', '_ZN7ProfileILj4EE5limitE': 'WEAK'}),
            ('local', {'_ZN7ProfileIN12_GLOBAL__N_13TagEE5limitE': 'LOCAL'}),
            ('strong-member', {'_ZN7ProfileILj2EE5limitE': 'GLOBAL'}),
        ):
            objects = tuple(f'static-{case}-{unit}.o' for unit in ('main', 'peer'))
            for unit, obj in zip(('main', 'peer'), objects):
                compile(static_members / case / f'{unit}.cpp', obj)
                table = symbols(obj)
                for name, binding in expected.items():
                    entry = table[name]
                    assert entry[4] == binding and entry[6] != 'UND', entry
            if case == 'strong-member':
                link(*objects, status=1, error="duplicate symbol '_ZN7ProfileILj2EE5limitE'")
            else:
                for order in (objects, objects[::-1]):
                    link(*order)
                    run('./program')

        run(host_cxx, '-std=c++20', '-O0', '-fno-pie', '-c', static_members / 'address/peer.cpp',
            '-o', 'host-static-address-peer.o')
        run(host_cxx, '-no-pie', 'static-address-main.o', 'host-static-address-peer.o', '-o', 'host-program')
        run('./host-program')

        compile(static_members / 'enum-nttp.cpp', 'static-enum.o')
        table = symbols('static-enum.o')
        for name, binding in (('_ZN14PrivateProfileILN12_GLOBAL__N_18LocalTagE11EE5limitE', 'LOCAL'),
                              ('_ZN13PublicProfileIL9PublicTag29EE5limitE', 'WEAK')):
            entry = table[name]
            assert entry[4] == binding and entry[6] != 'UND', entry
        link('static-enum.o')
        run('./program')

        template_members = fixtures / 'inline-template-members'
        include = f'-I{template_members / "shared"}'
        public_spend = '_ZN6BudgetILj2EE5spendEv'
        for case, expected in (
            ('state', {public_spend: 'WEAK'}),
            ('static-address', {'_ZN6BudgetILj2EE7initialEv': 'WEAK', '_ZN6BudgetILj4EE7initialEv': 'WEAK'}),
            ('local', {'_ZN11LocalBudgetIN12_GLOBAL__N_13TagEE5spendEv': 'LOCAL'}),
            ('strong', {public_spend: 'GLOBAL'}),
        ):
            objects = tuple(f'member-{case}-{unit}.o' for unit in ('main', 'peer'))
            for unit, obj in zip(('main', 'peer'), objects):
                compile(template_members / case / f'{unit}.cpp', obj, include)
                table = symbols(obj)
                for name, binding in expected.items():
                    entry = table[name]
                    assert entry[4] == binding and entry[6] != 'UND', entry
            for order in (objects, objects[::-1]):
                if case == 'strong':
                    link(*order, status=1, error=f"duplicate symbol '{public_spend}'")
                else:
                    link(*order)
                    run('./program')

        for case in ('state', 'static-address'):
            host_peer = f'host-member-{case}-peer.o'
            run(host_cxx, '-std=c++20', '-O0', '-fno-pie', include, '-c', template_members / case / 'peer.cpp',
                '-o', host_peer)
            objects = (f'member-{case}-main.o', host_peer)
            for order in (objects, objects[::-1]):
                run(host_cxx, '-no-pie', *order, '-o', 'host-program')
                run('./host-program')

        compile(template_members / 'enum-nttp.cpp', 'member-enum.o')
        table = symbols('member-enum.o')
        for name, binding in (('_ZN13PrivateBudgetILN12_GLOBAL__N_18LocalTagE11EE6chargeEv', 'LOCAL'),
                              ('_ZN12PublicBudgetIL9PublicTag29EE6chargeEv', 'WEAK')):
            entry = table[name]
            assert entry[4] == binding and entry[6] != 'UND', entry
        link('member-enum.o')
        run('./program')


if __name__ == '__main__':
    main()
