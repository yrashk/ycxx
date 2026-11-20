"""Materialize and call ordinary x86-64 ELF function addresses across link boundaries."""

import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run


def main():
    ycxx, cc, readelf = sys.argv[1:]
    fixtures = Path(__file__).resolve().with_suffix('')
    own = fixtures.parents[2] / 'own' / 'function-pointers'
    env = dict(os.environ, LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='2')

    with tempfile.TemporaryDirectory(prefix='ycxx-function-addresses-') as temp:
        scope = Path(temp)

        def invoke(*args, error=None):
            return run(args, scope, env, error)

        def cxx(*args, error=None):
            return invoke(ycxx, 'cxx', '--target=x86_64-linux-gnu', *args, error=error)

        def compile(source, obj, *flags):
            cxx('-c', *flags, fixtures / source, '-o', obj)

        def native(*inputs, error=None):
            output = scope / 'native'
            output.unlink(missing_ok=True)
            cxx('-nostdlib', *inputs, '-o', output, error=error)
            if error is None:
                invoke(output)
            else:
                assert not output.exists()

        def check_relocations(obj, symbol, *expected):
            output = invoke(readelf, '-Wr', obj)
            records = (line.split() for line in output.splitlines())
            actual = {(r[2], int(r[1], 16) & 0xffffffff, tuple(r[5:])) for r in records
                      if len(r) >= 7 and r[2].startswith('R_X86_64_') and r[4] == symbol}
            assert actual == set(expected), (obj, symbol, expected, output)

        repro = scope / 'repro.cpp'
        repro.write_text('int increment(int x) { return x + 1; }\n'
                         'int main() {\n'
                         '    int (*pointer)(int) = &increment;\n'
                         '    return pointer(41) == 42 ? 0 : 1;\n'
                         '}\n')
        native(repro)
        for test in ('init-pointer', 'reference', 'overload-resolution'):
            native(own / f'iota-{test}.run.cpp')

        compile('provider.cpp', 'provider.o')
        compile('consumer.cpp', 'consumer.o')
        compile('provider.cpp', 'weak.o', '-DWEAK_STEP')
        compile('consumer.cpp', 'expect-override.o', '-DEXPECT_STEP=9')
        got = ('R_X86_64_REX_GOTPCRELX', 42, ('-', '4'))
        plt = ('R_X86_64_PLT32', 4, ('-', '4'))
        pc = ('R_X86_64_PC32', 2, ('-', '4'))
        check_relocations('provider.o', 'external_step', got, plt)
        check_relocations('weak.o', 'external_step', got, plt)
        check_relocations('consumer.o', 'external_step', got)
        symbols = (line.split() for line in invoke(readelf, '-Ws', 'consumer.o').splitlines())
        local, = (s[7] for s in symbols if len(s) == 8 and s[3:5] == ['FUNC', 'LOCAL'])
        check_relocations('consumer.o', local, pc)

        for inputs in (('consumer.o', 'provider.o'), ('provider.o', 'consumer.o')):
            native(*inputs)
        invoke(ycxx, 'archive', '-o', 'libprovider.a', 'provider.o')
        native('consumer.o', 'libprovider.a')
        native('libprovider.a', 'consumer.o', error="undefined symbol 'external_step'")

        invoke(cc, '-fPIC', '-fno-asynchronous-unwind-tables', '-c', fixtures / 'override.c', '-o', 'override.o')
        native('consumer.o', 'weak.o')
        for inputs in (('weak.o', 'override.o'), ('override.o', 'weak.o')):
            native('expect-override.o', *inputs)
        invoke(cc, '-fPIC', '-fno-asynchronous-unwind-tables', '-DIFUNC_STEP', '-c', fixtures / 'override.c',
               '-o', 'ifunc.o')
        # Use the System V EI_OSABI so the native reader reaches symbol type validation.
        with (scope / 'ifunc.o').open('r+b') as obj:
            obj.seek(7)
            obj.write(b'\0')
        native('consumer.o', 'ifunc.o', error='unsupported type, size, binding or visibility for ELF symbol')

        invoke(cc, '-c', fixtures / 'undefined_weak.s', '-o', 'undefined-weak.o')
        check_relocations('undefined-weak.o', 'missing_weak', got)
        native('undefined-weak.o', error="R_X86_64_REX_GOTPCRELX for undefined weak symbol 'missing_weak' is unsupported")
        invoke(cc, '-pie', 'undefined-weak.o', '-o', 'system')
        invoke('./system')

        invoke(cc, '-shared', '-Wl,-z,defs', 'provider.o', '-o', 'libprovider.so')
        for mode, flags in (('-pie', '-fPIE'), ('-no-pie', '-fno-pie')):
            for part in ('consumer', 'provider'):
                invoke(cc, flags, '-O0', '-c', fixtures / f'{part}.cpp', '-o', f'host-{part}.o')
            for inputs in (('consumer.o', 'host-provider.o'), ('host-consumer.o', 'provider.o')):
                invoke(cc, mode, *inputs, '-o', 'system')
                invoke('./system')
            for inputs in (('consumer.o',), ('host-consumer.o',), ('expect-override.o', 'override.o')):
                invoke(cc, mode, '-rdynamic', *inputs, './libprovider.so', '-o', 'system')
                invoke('./system')
                if mode == '-no-pie' and inputs == ('host-consumer.o',):
                    symbols = invoke(readelf, '--dyn-syms', '-W', 'system')
                    entry, = (line.split() for line in symbols.splitlines() if line.split()[-1:] == ['external_step'])
                    assert int(entry[1], 16) != 0 and entry[3] == 'FUNC' and entry[6] == 'UND', symbols

        compile('alias.cpp', 'alias.o')
        check_relocations('alias.o', 'external_step', got)
        check_relocations('alias.o', 'step_alias', got)
        invoke(cc, '-shared', '-Wl,-z,defs', 'alias.o', 'override.o', '-o', 'libalias.so')
        invoke(cc, '-fPIE', '-pie', '-rdynamic', fixtures / 'alias_main.c', './libalias.so', '-o', 'system')
        invoke('./system')


if __name__ == '__main__':
    main()
