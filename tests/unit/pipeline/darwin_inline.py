"""Resolve external inline functions and variables across arm64 Mach-O objects (#124).

The Darwin counterpart of linux_inline.py, over the same fixtures: ycxx emits the eligible
inline definitions as weak (and private-external when hidden), and both ycxx's linker and the
system linker keep one definition in either object order.
"""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    ycxx, host_cxx, nm, ar = sys.argv[1:]
    fixtures = Path(__file__).resolve().parent
    env = dict(os.environ, LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='1')
    with tempfile.TemporaryDirectory(prefix='ycxx-inline-', dir='.') as temp:
        def run(*args, status=0, error=''):
            result = subprocess.run(args, cwd=temp, env=env, capture_output=True, text=True, timeout=60)
            if result.returncode != status or error not in result.stderr:
                raise AssertionError(f'{args} exited {result.returncode}, expected {status}'
                                     f' and stderr containing {error!r}\n{result.stdout}{result.stderr}')
            return result.stdout

        def compile(source, obj, *flags):
            run(ycxx, 'cxx', *flags, '-c', source, '-o', obj)

        def symbol_entries(obj):
            """Yield (name, (binding, defined)) for each symbol reported by `nm -m`."""
            for line in run(nm, '-m', obj).splitlines():
                m = re.match(r'^(?:[0-9a-f]+)?\s*\(([^)]*)\)\s+((?:weak )?(?:private )?(?:non-)?external)'
                             r'(?: \[[^]]*\])?\s+(\S+)', line)
                if m:
                    yield m[3], (m[2], m[1] != 'undefined')

        def symbols(obj):
            return dict(symbol_entries(obj))

        def links(*objects, args=()):
            """Links with ycxx's linker and with the system's (also dead-stripping), in both
            orders, and runs each."""
            for order in (objects, objects[::-1]):
                run(ycxx, 'cxx', *order, '-o', 'program')
                run('./program', *args)
                for strip in ((), ('-Wl,-dead_strip',)):
                    run(host_cxx, *order, *strip, '-o', 'host-program')
                    run('./host-program', *args)

        weak, strong = ('weak external', True), ('external', True)
        addresses = fixtures / 'object-address-members'
        shared = '__ZN6config5ProbeIXadL_ZNS_6sharedEEEE4readEv'
        local = '__ZN6config5ProbeIXadL_ZL13private_valueEEE4readEv'
        for part in ('main', 'peer'):
            obj = f'object-address-{part}.o'
            compile(addresses / f'{part}.cpp', obj)
            table = symbols(obj)
            assert table.get(local) == ('non-external', True), (obj, table)
            assert table.get(shared) == ('external', part == 'main'), (obj, table)
            run(host_cxx, '-std=c++20', '-pedantic-errors', '-O0', '-c', addresses / f'{part}.cpp',
                '-o', f'host-object-address-{part}.o')
        links('object-address-main.o', 'object-address-peer.o')
        links('object-address-main.o', 'host-object-address-peer.o')
        links('host-object-address-main.o', 'object-address-peer.o')

        # The assembly label gives g the linker spelling of f. The inline definition is weak,
        # and the linker must resolve the call to the strong definition of g (#694).
        (Path(temp) / 'same-spelling.cpp').write_text('''\
inline int f() { return 1; }
int g() __asm("__Z1fv");
int g() { return 2; }
int main() { return g() - 2; }
''')
        compile('same-spelling.cpp', 'same-spelling.o')
        bindings = [binding for name, binding in symbol_entries('same-spelling.o') if name == '__Z1fv']
        assert sorted(bindings) == sorted([weak, strong]), bindings
        run(ycxx, 'cxx', 'same-spelling.o', '-o', 'program')
        run('./program')

        hidden = fixtures / 'inline-hidden'
        weak_hidden = ('weak private external', True)
        for part, expected in {
            'main': {'__Z11hidden_stepi': weak_hidden, '__Z12hidden_twicei': weak_hidden},
            'peer': {'__Z11hidden_stepi': weak_hidden, '__Z12hidden_twicei': weak_hidden},
            'boundaries': {
                '__Z16hidden_from_decli': ('private external', True),
                '__Z15hidden_from_defi': ('private external', True),
                '__Z13strong_hiddeni': ('private external', True),
                '__Z14inline_defaulti': weak,
                '__Z8overloadi': ('private external', True),
                '__Z8overloadl': strong,
                '__Z14unrelated_attri': strong,
            },
            'declarator_visibility.accept': {'__Z4calli': strong},
        }.items():
            obj = f'hidden-{part}.o'
            compile(hidden / f'{part}.cpp', obj)
            table = symbols(obj)
            for name, binding in expected.items():
                assert table.get(name) == binding, (obj, name, binding, table.get(name))
            assert '__Z12static_locali' not in table or table['__Z12static_locali'][0] == 'non-external', table
        compile(hidden / 'boundaries_main.cpp', 'hidden-boundaries_main.o')
        links('hidden-main.o', 'hidden-peer.o')
        links('hidden-boundaries.o', 'hidden-boundaries_main.o')

        odr = fixtures / 'inline-odr'
        inline_symbols = ('__Z12inline_firsti', '__Z12inline_lateri', '__ZN9Operation6memberEi',
                          '__ZN14DeclaredMember6memberEi', '__Z15implicit_inlinei')
        for name in ('main', 'peer'):
            compile(odr / f'{name}.cpp', f'{name}.o')
            table = symbols(f'{name}.o')
            for symbol in inline_symbols:
                assert table[symbol] == weak, (name, symbol, table[symbol])
            assert table[f'__Z17{name}_direct_callsi'] == strong, table
        links('main.o', 'peer.o', args=('one', 'two'))
        # A second producer: the host compiler's peer, linked by both linkers.
        run(host_cxx, '-std=c++20', '-O0', '-c', odr / 'peer.cpp', '-o', 'host-peer.o')
        links('main.o', 'host-peer.o', args=('one', 'two'))
        # An archive member, through both linkers.
        run(ar, 'crs', 'libinline.a', 'peer.o')
        run(host_cxx, 'main.o', 'libinline.a', '-o', 'host-program')
        run('./host-program', 'one', 'two')
        run(ycxx, 'cxx', 'main.o', 'libinline.a', '-o', 'program')
        run('./program', 'one', 'two')

        friend = fixtures / 'inline-friend'
        for name in ('main', 'peer'):
            compile(friend / f'{name}.cpp', f'friend-{name}.o')
            table = symbols(f'friend-{name}.o')
            for symbol in ('__ZN14inline_address4readERKNS_4ItemE', '__ZN14inline_addressplERKNS_4ItemEi'):
                assert table[symbol] == weak, (name, symbol, table[symbol])
        links('friend-main.o', 'friend-peer.o')

        data = fixtures / 'inline-data'
        data_sections = {'__ZN4data11initializedE': '__DATA,__data', '__ZN4data16zero_initializedE': '__DATA,__data',
                         '__ZN4data9immutableE': '__TEXT,__const', '__ZN4data10redeclaredE': '__DATA,__data',
                         '__ZN4data11specializedIiEE': '__DATA,__data', '__ZN6Record4seedE': '__TEXT,__const',
                         '__ZN6Record10descriptorE': '__TEXT,__const'}
        for name in ('main', 'peer'):
            obj = f'data-{name}.o'
            compile(data / f'{name}.cpp', obj)
            listing = run(nm, '-m', obj)
            for symbol, section in data_sections.items():
                assert re.search(rf'\({re.escape(section)}\) weak external {symbol}$', listing, re.M), (obj, symbol)
            local = '__ZN12_GLOBAL__N_111LocalRecord4seedE'
            assert re.search(rf'\(__TEXT,__const\) non-external {local}$', listing, re.M), (obj, local)
        links('data-main.o', 'data-peer.o')
        run(host_cxx, '-std=c++20', '-O0', '-c', data / 'peer.cpp', '-o', 'host-data-peer.o')
        links('data-main.o', 'host-data-peer.o')

        assignment = fixtures / 'inline-assignment'
        for name in ('main', 'peer'):
            compile(assignment / f'{name}.cpp', f'assignment-{name}.o')
            assert symbols(f'assignment-{name}.o')['__ZN1HaSERKS_'] == weak, name
        links('assignment-main.o', 'assignment-peer.o')

        destructors = fixtures / 'inline-destructors'
        for fixture, binding, names in (
            ('shared', weak, ('__ZN6MemberIiED1Ev', '__ZN6MemberIbED1Ev', '__ZN5OwnerD1Ev')),
            ('local', ('non-external', True), ('__ZN6MemberIN12_GLOBAL__N_13TagEED1Ev',
                                             '__ZN5OwnerIN12_GLOBAL__N_13TagEED1Ev')),
        ):
            first, second = f'{fixture}-dtor-first.o', f'{fixture}-dtor-second.o'
            compile(destructors / fixture / 'first.cpp', first)
            compile(destructors / fixture / 'second.cpp', second)
            for obj in (first, second):
                table = symbols(obj)
                for name in names:
                    assert table.get(name) == binding, (obj, name, table.get(name))
            links(first, second)

        compile(destructors / 'enum-value.cpp', 'enum-dtor.o')
        table = symbols('enum-dtor.o')
        assert table.get('__ZN6MemberILN12_GLOBAL__N_13TagE7EED1Ev') == ('non-external', True), table
        assert table.get('__ZN12PublicMemberIL9PublicTag7EED1Ev') == weak, table
        run(ycxx, 'cxx', 'enum-dtor.o', '-o', 'program')
        run('./program')

        ordinary = destructors / 'out-of-line'
        compile(ordinary / 'first.cpp', 'strong-dtor-first.o')
        compile(ordinary / 'second.cpp', 'strong-dtor-second.o')
        assert symbols('strong-dtor-first.o')['__ZN5OwnerD1Ev'] == strong
        links('strong-dtor-first.o', 'strong-dtor-second.o')
        compile(ordinary / 'second.cpp', 'duplicate-dtor.o', '-DDUPLICATE_DESTRUCTOR')
        run(ycxx, 'cxx', 'strong-dtor-first.o', 'duplicate-dtor.o', '-o', 'program', status=1,
            error='duplicate symbol __ZN5OwnerD1Ev')

        specialized = destructors / 'specialized'
        symbol = '__ZN4CellIiED1Ev'
        for name, flags, binding in (
            ('written', ('-DWRITTEN_INLINE',), weak),
            ('declared', ('-DDECLARED_INLINE',), weak),
            ('strong', (), strong),
        ):
            main_obj, peer_obj = f'{name}-specialized-main.o', f'{name}-specialized-peer.o'
            compile(specialized / 'main.cpp', main_obj, *flags)
            compile(specialized / 'peer.cpp', peer_obj, *flags)
            for obj in (main_obj, peer_obj):
                entry = symbols(obj).get(symbol)
                assert entry == binding, (obj, symbol, entry)
            if binding == strong:
                run(ycxx, 'cxx', main_obj, peer_obj, '-o', 'program', status=1,
                    error=f'duplicate symbol {symbol}')
            else:
                links(main_obj, peer_obj)

        # Shared inline definitions beside every other kind of content: the split sections and
        # dead stripping keep string literals, initializers, guarded statics and pointers intact.
        mixed = fixtures / 'inline-mixed'
        for name in ('a', 'b'):
            compile(mixed / f'{name}.cpp', f'mixed-{name}.o')
            table = symbols(f'mixed-{name}.o')
            for symbol in ('__Z9shared_fni', '_table', '_counter', '_zeros', '__Z12shared_twicel'):
                assert table.get(symbol) == weak, (name, symbol, table.get(symbol))
        links('mixed-a.o', 'mixed-b.o')

        # One unit's copy is hidden, the other's default: the program exports one weak definition
        # whichever comes first, as the system linker does.
        (Path(temp) / 'vis-hidden.cpp').write_text(
            '[[gnu::visibility("hidden")]] inline int f(int x) { return x + 1; }\n'
            'int (*pa)(int) = f;\nextern int (*pb)(int);\n'
            'int main() { return pa == pb && pa(1) == 2 ? 0 : 1; }\n')
        (Path(temp) / 'vis-default.cpp').write_text('inline int f(int x) { return x + 1; }\nint (*pb)(int) = f;\n')
        compile('vis-hidden.cpp', 'vis-hidden.o')
        compile('vis-default.cpp', 'vis-default.o')
        assert symbols('vis-hidden.o')['__Z1fi'] == weak_hidden
        assert symbols('vis-default.o')['__Z1fi'] == weak
        links('vis-hidden.o', 'vis-default.o')
        for order in (('vis-hidden.o', 'vis-default.o'), ('vis-default.o', 'vis-hidden.o')):
            run(ycxx, 'cxx', *order, '-o', 'program')
            assert symbols('program')['__Z1fi'] == weak, (order, symbols('program')['__Z1fi'])

        # Not yet coalesced on Mach-O (#124): they stay strong rather than become wrongly weak.
        (Path(temp) / 'stays-strong.cpp').write_text('''
inline int with_static() { static int n = 0; return ++n; }
inline int with_lambda(int x) { return [x] { return x + 1; }(); }
template<class T> T twice(T x) { return x + x; }
template<> long twice<long>(long x) { return x * 2; }
extern "C" int labelled(int x) __asm("_renamed");
int labelled(int x) { return x; }
int use() { return with_static() + with_lambda(1) + twice(2) + int(twice(2L)) + labelled(0); }
''')
        compile('stays-strong.cpp', 'stays-strong.o')
        table = symbols('stays-strong.o')
        for name in ('__Z11with_staticv', '__Z11with_lambdai', '__Z5twiceIiET_S0_', '__Z5twiceIlET_S0_', '_renamed'):
            assert table.get(name) == strong, (name, table.get(name))

        # Controls: ordinary definitions stay strong and collide; ineligible inline variables
        # (internal, relocated, explicitly specialized without `inline`) are not weak.
        for name in ('strong1', 'strong2'):
            text = 'int ordinary(int value) { return value; }\n'
            if name == 'strong1':
                text += 'int main() { return ordinary(0); }\n'
            (Path(temp) / f'{name}.cpp').write_text(text)
            compile(f'{name}.cpp', f'{name}.o')
        run(ycxx, 'cxx', 'strong1.o', 'strong2.o', '-o', 'program', status=1, error='duplicate symbol __Z8ordinaryi')
        compile(data / 'bindings.cpp', 'data-controls.o')
        table = symbols('data-controls.o')
        assert table['__ZN6Public6scalarE'] == weak, table['__ZN6Public6scalarE']
        for name in ('_ordinary_constant', '_local_inline', '__ZN6Public1pE', '__ZN6Public1kE'):
            assert table[name] == ('non-external', True), (name, table[name])
        for name in ('_external_constant', '_backing', '_relocated', '__ZN8Ordinary5valueE',
                     '__Z11specializedIiE', '__Z11specializedI9NamedTypeE'):
            assert table[name] == strong, (name, table[name])
        # Without a weak definition the object makes no claim about splitting its sections.
        flags = run('otool', '-hv', 'strong2.o').splitlines()[-1]
        assert 'SUBSECTIONS_VIA_SYMBOLS' not in flags, flags
        assert 'SUBSECTIONS_VIA_SYMBOLS' in run('otool', '-hv', 'main.o').splitlines()[-1]

        static_members = fixtures / 'inline-template-statics'
        for case, expected in (
            ('constant', {}),
            ('address', {'__ZN7ProfileILj2EE5limitE': weak, '__ZN7ProfileILj4EE5limitE': weak}),
            ('local', {'__ZN7ProfileIN12_GLOBAL__N_13TagEE5limitE': ('non-external', True)}),
            ('strong-member', {'__ZN7ProfileILj2EE5limitE': strong}),
        ):
            objects = tuple(f'static-{case}-{unit}.o' for unit in ('main', 'peer'))
            for unit, obj in zip(('main', 'peer'), objects):
                compile(static_members / case / f'{unit}.cpp', obj)
                table = symbols(obj)
                for name, binding in expected.items():
                    assert table.get(name) == binding, (obj, name, table.get(name))
            if case == 'strong-member':
                run(ycxx, 'cxx', *objects, '-o', 'program', status=1, error='duplicate symbol __ZN7ProfileILj2EE5limitE')
            else:
                links(*objects)

        compile(static_members / 'enum-nttp.cpp', 'static-enum.o')
        table = symbols('static-enum.o')
        for name, binding in (('__ZN14PrivateProfileILN12_GLOBAL__N_18LocalTagE11EE5limitE', ('non-external', True)),
                              ('__ZN13PublicProfileIL9PublicTag29EE5limitE', weak)):
            assert table.get(name) == binding, (name, table.get(name))
        run(ycxx, 'cxx', 'static-enum.o', '-o', 'program')
        run('./program')


if __name__ == '__main__':
    main()
