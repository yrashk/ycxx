"""Exchange System V AMD64 calls between ycxx and native ELF objects."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def run(args, cwd, env, error=None):
    result = subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True, timeout=30)
    if error is not None:
        if result.returncode != 0 and error in result.stderr:
            return result.stdout
    elif result.returncode == 0:
        return result.stdout
    raise AssertionError(f'{args} exited {result.returncode}\n{result.stdout}{result.stderr}')


def external_data_addresses(compile, target, source_dir, scope, cold, cc, readelf, ar):
    for source in (source_dir / 'linux_external_data').iterdir():
        shutil.copyfile(source, scope / source.name)
    for part in ('consumer', 'provider'):
        compile(target, '-c', f'{part}.cpp', '-o', f'data-{part}.o')

    def native(*inputs):
        compile(target, '-nostdlib', *inputs, '-o', 'data-native')
        run(['./data-native'], scope, cold)

    for inputs in (('data-consumer.o', 'data-provider.o'), ('data-provider.o', 'data-consumer.o'),
                   ('consumer.cpp', 'provider.cpp')):
        native(*inputs)
    run([ar, 'rcs', 'data-provider.a', 'data-provider.o'], scope, cold)
    native('data-consumer.o', 'data-provider.a')

    output = run([readelf, '-Wr', 'data-consumer.o'], scope, cold)
    records = [line.split() for line in output.splitlines() if 'R_X86_64_' in line]
    got = [r for r in records if r[2] == 'R_X86_64_REX_GOTPCRELX']
    pointers = {tuple(r[4:]) for r in records if r[2] == 'R_X86_64_64'}
    assert {r[4] for r in got} == {'_ZN4data6scalarE', '_ZN4data7numbersE', '_ZN4data6recordE'}, output
    assert all(int(r[1], 16) & 0xffffffff == 42 and r[5:] == ['-', '4'] for r in got), output
    assert {('_ZN4data6scalarE', '+', '0'), ('_ZN4data7numbersE', '+', '8')} <= pointers, output

    for mode, code in (('-no-pie', '-fno-pie'), ('-pie', '-fPIE')):
        for part in ('consumer', 'provider'):
            run([cc, '-std=c++20', '-O2', code, '-c', f'{part}.cpp', '-o', f'data-host-{part}.o'], scope, cold)
        for inputs in (('data-consumer.o', 'data-host-provider.o'), ('data-host-provider.o', 'data-consumer.o'),
                       ('data-host-consumer.o', 'data-provider.o'), ('data-provider.o', 'data-host-consumer.o')):
            run([cc, mode, *inputs, '-o', 'data-system'], scope, cold)
            run(['./data-system'], scope, cold)

    (scope / 'same-data.cpp').write_text('int bss_value; static int local_value = 3;\n'
                                         'int* address_of_bss() { return &bss_value; }\n'
                                         'int main() { if (*address_of_bss() != 0) return 1;\n'
                                         '  *address_of_bss() = 41; return bss_value + local_value != 44; }\n')
    compile(target, '-c', 'same-data.cpp')
    output = run([readelf, '-Ws', '-Wr', 'same-data.o'], scope, cold)
    assert 'R_X86_64_REX_GOTPCRELX' in output and 'R_X86_64_PC32' in output, output
    native('same-data.o')

    (scope / 'host-data.c').write_text('int host_data = 51;\n')
    (scope / 'host-data-user.cpp').write_text('extern "C" int host_data;\n'
                                              'int main() { return *(&host_data) != 51; }\n')
    run([cc, '-c', 'host-data.c'], scope, cold)
    native('host-data-user.cpp', 'host-data.o')
    compile(target, '-c', 'host-data-user.cpp')
    compile(target, '-nostdlib', 'host-data-user.o', '-o', 'missing',
            error="undefined symbol 'host_data'")
    assert not (scope / 'missing').exists()

    (scope / 'interposable.cpp').write_text('extern "C" {\n'
                                             'int binding_slot = 12;\n'
                                             'int* slot_address() { return &binding_slot; }\n'
                                             '}\n')
    (scope / 'interposer.c').write_text('int binding_slot = 42; int* slot_address(void);\n'
                                         'int main(void) {\n'
                                         '  int* p = slot_address(); if (p != &binding_slot || *p != 42) return 1;\n'
                                         '  *p = 58; return binding_slot != 58;\n'
                                         '}\n')
    compile(target, '-c', 'interposable.cpp')
    run([cc, '-shared', '-Wl,-soname,libinterposable.so', 'interposable.o', '-o', 'libinterposable.so'], scope, cold)
    run([cc, '-fPIE', '-pie', '-rdynamic', 'interposer.c', './libinterposable.so', '-Wl,-rpath,$ORIGIN',
         '-o', 'interposer'], scope, cold)
    run(['./interposer'], scope, cold)


def integer_aggregate_calls(compile, target, sources, scope, cold, cc, provider, client):
    for part, name in (('provider', provider), ('client', client)):
        source = sources / f'{name}.cpp'
        compile(target, '-c', source, '-o', f'aggregate-{part}.o')
        run([cc, '-std=c++26', '-O2', '-fno-pie', '-c', source, '-o', f'host-{part}.o'], scope, cold)
    compile(target, '-nostdlib', 'aggregate-provider.o', 'aggregate-client.o', '-o', 'aggregate-native')
    run(['./aggregate-native'], scope, cold)
    for inputs in (('aggregate-provider.o', 'host-client.o'), ('host-provider.o', 'aggregate-client.o')):
        run([cc, '-no-pie', *inputs, '-o', 'aggregate-mixed'], scope, cold)
        run(['./aggregate-mixed'], scope, cold)


def main():
    ycxx, cc, readelf, ar, libycxx_source = sys.argv[1:]
    source_dir = Path(__file__).resolve().parent
    libycxx = Path(libycxx_source)
    target = '--target=x86_64-pc-linux-gnu'
    with tempfile.TemporaryDirectory(prefix='ycxx-linux-') as temp, tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        shutil.copyfile(source_dir / 'linux_object.cpp', scope / 'source.cpp')
        shutil.copyfile(source_dir / 'linux_consumer.c', scope / 'consumer.c')
        base = {k: v for k, v in os.environ.items() if k not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT')}
        base.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')
        cold = dict(base, YCXX_NO_DAEMON='1')
        daemon = dict(base, YCXX_DAEMON='require')

        def compile(*args, env=cold, error=None):
            return run([ycxx, 'cxx', *args], scope, env, error)

        def symbol(path, name):
            table = run([readelf, '-Ws', path], scope, cold)
            entry, = (line.split() for line in table.splitlines() if line.split()[-1:] == [name])
            return int(entry[1], 16), int(entry[2]), entry[3:7]

        (scope / 'system-leaf.s').write_text('''\
.text
.globl system_leaf
.type system_leaf, @function
system_leaf:
    mov $42, %eax
    ret
.size system_leaf, .-system_leaf
.section .note.GNU-stack,"",@progbits
''')
        run([cc, '-c', 'system-leaf.s'], scope, cold)
        assert symbol('system-leaf.o', 'system_leaf') == (0, 6, ['FUNC', 'GLOBAL', 'DEFAULT', '1'])
        (scope / 'use-leaf.cpp').write_text('extern "C" int system_leaf();\n'
                                           'int main() { return system_leaf() != 42; }\n')
        compile(target, '-nostdlib', 'use-leaf.cpp', 'system-leaf.o', '-o', 'use-leaf')
        run(['./use-leaf'], scope, cold)

        leaf = 'extern "C" int leaf() { return 42; }\n'
        (scope / 'leaf.cpp').write_text(leaf)
        compile(target, '-c', 'leaf.cpp')
        sections = run([readelf, '-SW', 'leaf.o'], scope, cold)
        text, = (line.split(']', 1)[1].split() for line in sections.splitlines() if '] .text ' in line)
        leaf_size = int(text[4], 16)
        assert leaf_size > 0
        leaf_symbol = symbol('leaf.o', 'leaf')
        assert leaf_symbol == (0, leaf_size, ['FUNC', 'GLOBAL', 'DEFAULT', '1'])
        (scope / 'leaf-with-next.cpp').write_text(leaf + 'extern "C" int next() { return 1; }\n')
        compile(target, '-c', 'leaf-with-next.cpp')
        assert symbol('leaf-with-next.o', 'leaf') == leaf_symbol
        next_offset, next_size, next_info = symbol('leaf-with-next.o', 'next')
        assert next_offset > leaf_size and next_size > 0 and next_info == ['FUNC', 'GLOBAL', 'DEFAULT', '1']

        compile(target, '-c', 'source.cpp')
        output = run([readelf, '-h', '-Ws', '-Wr', 'source.o'], scope, cold)
        for field in ('REL (Relocatable file)', 'X86-64', 'R_X86_64_PLT32', 'R_X86_64_PC32', 'sum7', '_Z5twicel'):
            if field not in output:
                raise AssertionError(f'{field!r} missing in readelf output\n{output}')
        run([cc, 'consumer.c', 'source.o', '-o', 'consumer'], scope, cold)
        run(['./consumer'], scope, cold)

        (scope / 'init.cpp').write_text('static int produce() { return 17; }\n'
                                       'static const int value = produce();\n'
                                       'extern "C" int initialized() { return value; }\n')
        (scope / 'init-consumer.c').write_text('int initialized(void);\n'
                                              'int main(void) { return initialized() != 17; }\n')
        compile(target, '-c', 'init.cpp')
        output = run([readelf, '-SW', '-Wr', 'init.o'], scope, cold)
        assert 'INIT_ARRAY' in output and '.rela.init_array' in output and 'R_X86_64_64' in output
        run([cc, 'init-consumer.c', 'init.o', '-o', 'init-consumer'], scope, cold)
        run(['./init-consumer'], scope, cold)

        for name in ('linux_variadic.cpp', 'linux_variadic_peer.c', 'linux_variadic_entry.s'):
            shutil.copyfile(source_dir / name, scope / name)
        compile(target, '-c', 'linux_variadic.cpp')
        run([cc, 'linux_variadic_peer.c', 'linux_variadic_entry.s', 'linux_variadic.o',
             '-o', 'variadic'], scope, cold)
        run(['./variadic'], scope, cold)

        indirect_caller = 'indirect-variadic.accept.cpp'
        caller_source = source_dir.parents[1] / 'own' / 'function-pointers' / indirect_caller
        shutil.copyfile(caller_source, scope / indirect_caller)
        for source in (source_dir / 'linux_indirect_variadic').iterdir():
            shutil.copyfile(source, scope / source.name)
        compile(target, '-c', indirect_caller, '-o', 'indirect-caller.o')
        compile(target, '-c', 'indirect-variadic-control.accept.cpp', '-o', 'indirect-controls.o')
        run([cc, '-std=c++20', '-O2', '-c', 'indirect-variadic-provider.cpp', '-o', 'indirect-provider.o'],
            scope, cold)
        run([cc, 'indirect-variadic-witness.S', 'indirect-provider.o', 'indirect-caller.o',
             'indirect-controls.o', '-o', 'indirect-variadic'], scope, cold)
        run(['./indirect-variadic'], scope, cold)

        for name in ('abi.hpp', 'caller.cpp', 'callee.cpp', 'boundaries.cpp', 'boundaries.s'):
            name = f'linux_narrow_{name}'
            shutil.copyfile(source_dir / name, scope / name)
        for part in ('caller', 'callee'):
            source = f'linux_narrow_{part}.cpp'
            compile(target, '-c', source)
            run([cc, '-std=c++20', '-O2', '-c', source, '-o', f'native_{part}.o'], scope, cold)
        for caller, callee in (('linux_narrow_caller.o', 'native_callee.o'),
                               ('native_caller.o', 'linux_narrow_callee.o')):
            run([cc, caller, callee, '-o', 'narrow'], scope, cold)
            run(['./narrow'], scope, cold)
        compile(target, '-c', 'linux_narrow_boundaries.cpp')
        run([cc, 'linux_narrow_boundaries.s', 'linux_narrow_boundaries.o', '-o', 'narrow_boundaries'], scope, cold)
        run(['./narrow_boundaries'], scope, cold)

        for name in ('abi.hpp', 'caller.cpp', 'callee.cpp', 'host_variadic.cpp', 'carrier.s'):
            name = f'linux_bool_{name}'
            shutil.copyfile(source_dir / name, scope / name)
        bool_probes = ('-DBOOL417_VARIADIC_PROBE', '-DBOOL417_CARRIER_PROBE')
        for part in ('caller', 'callee'):
            source = f'linux_bool_{part}.cpp'
            compile(target, '-c', *bool_probes, source)
            run([cc, '-std=c++20', '-O2', '-c', *bool_probes, source,
                 '-o', f'native_bool_{part}.o'], scope, cold)
        run([cc, '-std=c++20', '-O2', '-c', 'linux_bool_host_variadic.cpp'], scope, cold)
        run([cc, '-c', 'linux_bool_carrier.s'], scope, cold)
        for caller, callee in (('linux_bool_caller.o', 'native_bool_callee.o'),
                               ('native_bool_caller.o', 'linux_bool_callee.o')):
            run([cc, caller, callee, 'linux_bool_host_variadic.o', 'linux_bool_carrier.o',
                 '-o', 'bool_abi'], scope, cold)
            run(['./bool_abi'], scope, cold)

        external_data_addresses(compile, target, source_dir, scope, cold, cc, readelf, ar)
        compile('-c', 'source.cpp', '-o', 'cold-darwin.o')
        try:
            compile(target, '-c', 'source.cpp', '-o', 'warm-first-linux.o', env=daemon)
            compile('-c', 'source.cpp', '-o', 'warm-darwin.o', env=daemon)
            compile('--target', 'x86_64-linux-gnu', '-c', 'source.cpp', '-o', 'warm-linux.o', env=daemon)
            elf = (scope / 'source.o').read_bytes()
            macho = (scope / 'cold-darwin.o').read_bytes()
            assert macho.startswith(bytes.fromhex('cffaedfe'))
            assert (scope / 'warm-darwin.o').read_bytes() == macho
            assert (scope / 'warm-first-linux.o').read_bytes() == elf
            assert (scope / 'warm-linux.o').read_bytes() == elf
            compile(target, '-c', 'consumer.cpp', '-o', 'warm-data.o', env=daemon)
            assert (scope / 'warm-data.o').read_bytes() == (scope / 'data-consumer.o').read_bytes()
            compile(target, '-nostdlib', 'warm-data.o', 'data-provider.a', '-o', 'data-warm', env=daemon)
            run(['./data-warm'], scope, cold)
        finally:
            run([ycxx, 'ctl', 'shutdown'], scope, base)

        parameters = 'only bool, 8-, 16-, 32- and 64-bit integer, pointer and reference parameters'
        results = 'only void, bool, 8-, 16-, 32- and 64-bit integer, pointer and reference results'
        for name in ('direct-results', 'explicit-results'):
            source = source_dir / 'deduced-results' / f'{name}.cpp'
            compile(target, '-nostdlib', source, '-o', name)
            run([f'./{name}'], scope, cold)
        aggregates = source_dir / 'linux_integer_aggregates'
        integer_aggregate_calls(compile, target, aggregates, scope, cold, cc, 'provider', 'client')
        integer_aggregate_calls(compile, target, aggregates, scope, cold, cc,
                                'pointer-aggregate-peer', 'pointer-aggregate-main')
        compile(target, '-isystem', libycxx / 'include', '-nostdlib',
                aggregates / 'pointer-aggregate-empty-list.cpp', libycxx / 'src/runtime/linkage/static.cpp',
                '-o', 'aggregate-empty-list')
        run(['./aggregate-empty-list'], scope, cold)
        (scope / 'packing.cpp').write_text(
            '#pragma pack(push, 1)\n'
            'struct Capped { char a; int b; };\n'
            'int read_reference(const Capped& x) { return x.b; }\n'
            '#pragma pack(pop)\n'
            'struct Popped { char a; int b; } unrelated __attribute__((aligned(32)));\n'
            'int read_popped(Popped x) { return x.b; }\n'
            '#pragma pack(push, label, 1)\n'
            'struct Uncertain { char a; int b; };\n'
            '#pragma pack()\n'
            'struct Reset { char a; int b; };\n'
            'int read_reset(Reset x) { return x.b; }\n'
            'template<class T> struct __attribute__((packed)) Chosen { char a; T b; };\n'
            'template<class T> struct Chosen<T*> { char a; int b; };\n'
            'int read_selected(Chosen<int*> x) { return x.b; }\n'
            'int main() { return read_popped({1, 10}) + read_reset({2, 12}) + read_selected({3, 20}) - 42; }\n')
        compile(target, '-nostdlib', 'packing.cpp', '-o', 'packing')
        run(['./packing'], scope, cold)
        assert '#pragma pack' in compile(target, '-E', 'packing.cpp')
        aggregate_parameter = 'unsupported aggregate parameter layout'
        aggregate_result = 'unsupported aggregate result layout'
        floating_varargs = 'floating-point parameters and results of variadic functions are unsupported'
        cases = (
            ('long double f(long double x) { return x; }', 'x87 long double parameters and results are unsupported'),
            ('struct X { double x; }; auto f() { return X{1.0}; }', aggregate_result),
            ('struct X { long a, b, c; }; int f(X) { return 1; }', aggregate_parameter),
            ('struct X {}; int f(X) { return 1; }', aggregate_parameter),
            ('struct X { double a; int b; }; int f(X x) { return x.b; }', aggregate_parameter),
            ('struct X { int a[2]; }; int f(X x) { return x.a[0]; }', aggregate_parameter),
            ('struct M { int a; }; struct X { M m; }; int f(X x) { return x.m.a; }', aggregate_parameter),
            ('struct B { int a; }; struct X : B { int b; }; int f(X x) { return x.b; }', aggregate_parameter),
            ('struct X { int a : 3; }; int f(X x) { return x.a; }', aggregate_parameter),
            ('union X { int a; unsigned b; }; int f(X x) { return x.a; }', aggregate_parameter),
            ('struct X { int a; ~X(); }; int f(X x) { return x.a; }', aggregate_parameter),
            ('struct __attribute__((packed)) X { char a; int b; }; int f(X x) { return x.b; }',
             aggregate_parameter),
            ('struct X [[gnu::packed]];\nstruct X { char a; int b; }; int f(X x) { return x.b; }',
             aggregate_parameter),
            ('#define PACKED [[gnu::packed]]\n'
             'struct X { char a; int b PACKED; }; int f(X x) { return x.b; }', aggregate_parameter),
            ('typedef int Low __attribute__((aligned(1))); using Alias = Low;\n'
             'struct X { char a; Alias b; }; int f(X x) { return x.b; }',
             aggregate_parameter),
            ('#pragma pack(push, 1)\nstruct X { char a; int b; };\n#pragma pack(pop)\n'
             'int f(X x) { return x.b; }', aggregate_parameter),
            ('#define PACK _Pragma("pack(push, label, 1)")\nPACK\n'
             'struct X { char a; int b; };\n#pragma pack()\nint f(X x) { return x.b; }',
             aggregate_parameter),
            ('struct X { char a;\n#pragma pack(1)\nint b;\n#pragma pack()\n};\n'
             'int f(X x) { return x.b; }', aggregate_parameter),
            ('template<class T> struct __attribute__((packed)) X { char a; T b; };\n'
             'int f(X<int> x) { return x.b; }', aggregate_parameter),
            ('template<class T> [[gnu::packed]] struct X;\n'
             'template<class T> struct X { char a; T b; };\n'
             'int f(X<int> x) { return x.b; }', aggregate_parameter),
            ('#pragma pack(push, 1)\ntemplate<class T> struct X { char a; T b; };\n'
             '#pragma pack(pop)\nint f(X<int> x) { return x.b; }', aggregate_parameter),
            ('template<class T> struct X { char a; T b [[gnu::packed]]; };\n'
             'int f(X<int> x) { return x.b; }', aggregate_parameter),
            ('template<class T> struct X { char a; T b; };\n'
             'template<class T> struct __attribute__((packed)) X<T*> { char a; int b; };\n'
             'int f(X<int*> x) { return x.b; }', aggregate_parameter),
            ('struct X { double a; }; int g(X); int f() { return g(X{1.0}); }',
             f'call: {aggregate_parameter}'),
            ('struct X { long a, b, c; }; X g(); long f() { X x = g(); return x.a; }',
             f'call: {aggregate_result}'),
            ('struct X { int a; }; int f(int (*g)(X)) { return g(X{1}); }', f'call: {parameters}'),
            ('struct X { int a; }; int f(X (*g)()) { X x = g(); return x.a; }', f'call: {results}'),
            ('struct X { int a; }; struct B { static int g(X); }; int f() { return B::g(X{1}); }',
             'call: unsupported fixed-prototype parameter type'),
            ('int f(double, ...) { return 1; }', floating_varargs),
            ('double f(int, ...) { return 1.0; }', floating_varargs),
            ('struct X { int x; }; int f(X, ...) { return 1; }', parameters),
            ('int f(int (*g)(double, ...)) { return g(1.0); }', f'call: {floating_varargs}'),
            ('int f(double (*g)(int, ...)) { return g(1) > 0.0; }', f'call: {floating_varargs}'),
            ('int f(int (*g)(int, ...)) { return g(1, 0.75); }',
             'call: floating-point ellipsis arguments are unsupported'),
            ('struct X { int x; }; int f(int (*g)(X, ...)) { return g(X{1}); }', f'call: {parameters}'),
            ('struct X { int x; }; int f(int (*g)(int, ...), X x) { return g(2, x); }',
             'call: only 32- and 64-bit integer and pointer ellipsis arguments'),
            ('int g(int, ...); int f(long double& x) { return g(1, x); }',
             'call: x87 long double ellipsis arguments are unsupported'),
            ('struct X { int x; }; int g(X, ...); int f() { return g(X{1}); }',
             'call: unsupported direct variadic parameter type'),
            ('struct X { int x; }; int g(int, ...); int f() { X x{1}; return g(1, x); }',
             'call: only double, 32- and 64-bit integer, and pointer ellipsis arguments'),
        )
        for source, error in cases:
            (scope / 'unsupported.cpp').write_text(source + '\n')
            compile(target, '-fsyntax-only', 'unsupported.cpp')
            compile(target, '-c', 'unsupported.cpp', error=f'x86-64 System V ABI: {error}')
            assert not (scope / 'unsupported.o').exists()
        (scope / 'unsupported.cpp').write_text('int f() { return sizeof(__builtin_infl()); }\n')
        compile(target, '-fsyntax-only', 'unsupported.cpp', error='extended floating-point types are not supported')
        (scope / 'unsupported.cpp').write_text('long double x{};\n')
        compile(target, '-fsyntax-only', 'unsupported.cpp', error='the expression cannot be evaluated yet')
        (scope / 'unsupported.cpp').write_text('struct Box { long double value; }; Box box{};\n')
        compile(target, '-fsyntax-only', 'unsupported.cpp')
        compile(target, '-c', 'unsupported.cpp', error='an extended floating-point format is not supported yet')
        for body in ('static long double x{}; return sizeof(x);', 'static long double x; ++x; return 0;'):
            (scope / 'unsupported.cpp').write_text(f'int f() {{ {body} }}\n')
            compile(target, '-fsyntax-only', 'unsupported.cpp')
            compile(target, '-c', 'unsupported.cpp', error='an extended floating-point format is not supported yet')
        (scope / 'storage.cpp').write_text('int f() { static long double x; return sizeof(x); }\n')
        compile(target, '-c', 'storage.cpp')


if __name__ == '__main__':
    main()
