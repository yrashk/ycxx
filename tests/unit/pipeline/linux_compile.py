"""Exchange scalar System V AMD64 calls between ycxx and native ELF objects."""

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


def main():
    ycxx, cc, readelf = sys.argv[1:]
    source_dir = Path(__file__).resolve().parent
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
        finally:
            run([ycxx, 'ctl', 'shutdown'], scope, base)

        parameters = 'only bool, 8-, 16-, 32- and 64-bit integer, pointer and reference parameters'
        results = 'only void, bool, 8-, 16-, 32- and 64-bit integer, pointer and reference results'
        cases = (
            ('double f(double x) { return x; }', results),
            ('long double f(long double x) { return x; }', results),
            ('struct X { int x; }; int f(X x) { return x.x; }', parameters),
            ('struct X { int x; }; int g(X); int f() { return g(X{1}); }',
             f'call: {parameters}'),
            ('struct X { int x; }; X g(int); int f() { X x = g(1); return x.x; }',
             f'call: {results}'),
            ('int f(int, ...) { return 1; }', 'variadic functions'),
            ('int f(int (*g)(int, ...)) { return g(1); }', 'call: variadic functions'),
            ('int f(int (*g)(int, ...)) { return g(1, 2); }', 'call: variadic functions'),
            ('int g(double, ...); int f() { return g(1.0); }', f'call: {parameters}'),
            ('double g(int, ...); int f() { return g(1); }', f'call: {results}'),
            ('int g(int, ...); int f() { return g(1, 1.0f); }',
             'call: only 32- and 64-bit integer and pointer ellipsis arguments'),
            ('struct X { int x; }; int g(X, ...); int f() { return g(X{1}); }',
             f'call: {parameters}'),
            ('struct X { int x; }; int g(int, ...); int f() { X x{1}; return g(1, x); }',
             'call: only 32- and 64-bit integer and pointer ellipsis arguments'),
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
        (scope / 'unsupported.cpp').write_text('extern int g; int f() { return g; }\n')
        compile(target, '-c', 'unsupported.cpp', error='symbol address needs a direct RIP-relative reference')
        assert not (scope / 'unsupported.o').exists()


if __name__ == '__main__':
    main()
