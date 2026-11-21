"""Exchange scalar SSE calls with native System V AMD64 objects."""

import os
from pathlib import Path
import shutil
import sys
import tempfile

from linux_compile import run


def main():
    ycxx, host_cxx, libycxx_source = sys.argv[1:]
    libycxx = Path(libycxx_source)
    sources = Path(__file__).resolve().parent / 'linux_scalar_sse'
    target = '--target=x86_64-pc-linux-gnu'
    with tempfile.TemporaryDirectory(prefix='ycxx-sse-') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        for source in sources.iterdir():
            shutil.copyfile(source, scope / source.name)
        base = {k: v for k, v in os.environ.items() if k not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT')}
        base.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')
        cold = dict(base, YCXX_NO_DAEMON='1')
        daemon = dict(base, YCXX_DAEMON='require')

        def compile(*args, env=cold, error=None):
            return run([ycxx, 'cxx', target, *args], scope, env, error)

        try:
            for fixture in ('scalar-sse', 'record-sse'):
                for role in ('caller', 'provider'):
                    source = f'{fixture}-{role}.cpp'
                    compile('-c', source, '-o', f'ycxx-{role}.o')
                    compile('-c', source, '-o', f'warm-{role}.o', env=daemon)
                    assert (scope / f'warm-{role}.o').read_bytes() == (scope / f'ycxx-{role}.o').read_bytes()
                for opt in ('-O0', '-O2'):
                    for role in ('host', 'caller', 'provider'):
                        run([host_cxx, '-std=c++26', '-pedantic-errors', opt, '-fno-lto', '-c',
                             f'{fixture}-{role}.cpp', '-o', f'native-{role}.o'], scope, cold)
                    for caller, provider in (('ycxx', 'native'), ('native', 'ycxx'), ('warm', 'warm')):
                        run([host_cxx, 'native-host.o', f'{caller}-caller.o', f'{provider}-provider.o',
                             '-o', 'mixed'], scope, cold)
                        run(['./mixed'], scope, cold)

            for fixture in ('variadic-caller', 'variadic-al-caller'):
                compile('-c', f'{fixture}.cpp', '-o', f'{fixture}.o')
                compile('-c', f'{fixture}.cpp', '-o', f'{fixture}-warm.o', env=daemon)
                assert (scope / f'{fixture}.o').read_bytes() == (scope / f'{fixture}-warm.o').read_bytes()
            for opt in ('-O0', '-O2'):
                run([host_cxx, '-std=c++26', '-pedantic-errors', opt, '-fno-lto',
                     'variadic-consumer.cpp', 'variadic-caller.o', '-o', 'mixed-variadic'], scope, cold)
                run(['./mixed-variadic'], scope, cold)
            run([host_cxx, 'variadic-al-observer.S', 'variadic-al-caller.o', '-o', 'observe-al'], scope, cold)
            run(['./observe-al'], scope, cold)
            run(['./observe-al', 'argument'], scope, cold)

            (scope / 'printf.cpp').write_text('extern "C" int printf(const char*, ...);\n'
                                              'int main() { float f = 1.5f; printf("%f\\n", f); }\n')
            compile('-c', 'printf.cpp')
            run([host_cxx, 'printf.o', '-o', 'printf-float'], scope, cold)
            assert run(['./printf-float'], scope, cold) == '1.500000\n'

            includes = ('-isystem', libycxx / 'include')
            compile(*includes, '-c', libycxx / 'src/runtime/linkage/static.cpp', '-o', 'linkage.o')
            for mode, env in (('cold', cold), ('warm', daemon)):
                compile(*includes, '-nostdlib', 'freestanding.cpp', 'linkage.o', '-o', mode, env=env)
                run([f'./{mode}'], scope, cold)
            assert (scope / 'warm').read_bytes() == (scope / 'cold').read_bytes()
        finally:
            run([ycxx, 'ctl', 'shutdown'], scope, base)

        unresolved_copy = ('struct Leaf { volatile int value; };\n'
                           'struct Wrapper { Leaf leaf; Wrapper(const Wrapper&) = default; };\n')
        cases = (
            ('float f(float a, float b) { return a + b; }', '`fadd`: floating-point arithmetic'),
            ('bool f(float a, float b) { return a == b; }', '`fcmp`: floating-point arithmetic'),
            ('float f(double x) { return x; }', '`fptrunc`: floating-point arithmetic'),
            ('int f(double x) { return static_cast<int>(x); }', '`fptosi`: floating-point arithmetic'),
            ('float f(float (*g)(float)) { return g(1.0F); }',
             'call: floating-point parameters and results of indirect calls are unsupported'),
            (unresolved_copy + 'double f(Wrapper, double value) { return value; }',
             'defaulted copy ABI classification is unavailable for a parameter'),
            (unresolved_copy + 'Wrapper f(double) { for (;;) {} }',
             'defaulted copy ABI classification is unavailable for the result'),
        )
        for source, error in cases:
            (scope / 'unsupported.cpp').write_text(source + '\n')
            compile('-fsyntax-only', 'unsupported.cpp')
            compile('-c', 'unsupported.cpp', error=error)
            assert not (scope / 'unsupported.o').exists()


if __name__ == '__main__':
    main()
