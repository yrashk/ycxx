"""Compile System V AMD64 variadic definitions and exchange cursors with a host peer."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    ycxx, host_cxx, libycxx_source = sys.argv[1:]
    libycxx = Path(libycxx_source)
    includes = ('-isystem', libycxx / 'include')
    source_dir = Path(__file__).resolve().parent / 'linux_variadic_definitions'
    linux = '--target=x86_64-linux-gnu'
    darwin = '--target=arm64-apple-darwin'
    with tempfile.TemporaryDirectory(prefix='ycxx-varargs-') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        for source in source_dir.glob('*.cpp'):
            shutil.copyfile(source, scope / source.name)
        base = {k: v for k, v in os.environ.items() if k not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT')}
        base.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')
        cold = dict(base, YCXX_NO_DAEMON='1')
        daemon = dict(base, YCXX_DAEMON='require')

        def run(*args, env=cold, error=None):
            result = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=True, timeout=30)
            if error is None:
                assert result.returncode == 0, (args, result.stdout, result.stderr)
            else:
                assert result.returncode != 0 and error in result.stderr, (args, result.stdout, result.stderr)
            return result.stderr

        for source in ('integer-varargs.cpp', 'zero-fixed-varargs.cpp'):
            run(ycxx, 'cxx', linux, '-nostdlib', source, '-o', 'native')
            run('./native')

        # The pinned libycxx header references its runtime's static linkage guard.
        run(ycxx, 'cxx', linux, *includes, '-c',
            libycxx / 'src/runtime/linkage/static.cpp', '-o', 'linkage.o')
        for source in ('cstdarg-discarded-arguments.cpp', 'cstdarg-no-named.cpp'):
            run(ycxx, 'cxx', linux, *includes, '-nostdlib', source, 'linkage.o', '-o', 'native')
            run('./native')

        for callee, driver, linkage in (('host-double-callee.cpp', 'host-double-driver.cpp', ()),
                                        ('cstdarg-wrapper.cpp', 'cstdarg-host.cpp', ('linkage.o',))):
            run(ycxx, 'cxx', linux, *includes, '-c', callee, '-o', 'callee.o')
            for mode in ('-O0', '-O2'):
                run(host_cxx, '-std=c++26', mode, '-no-pie', driver, 'callee.o', *linkage, '-o', 'mixed')
                run('./mixed')

        rejected = 'nonvariadic-va-start.cpp'
        args = (ycxx, 'cxx', linux, '-fsyntax-only', rejected)
        expected = 'va_start requires an enclosing variadic function'
        plain = run(*args, error=expected)
        assert f'{rejected}:10:' in plain, plain
        for _ in range(2):
            assert run(*args, env=daemon, error=expected) == plain

        invalid = (
            ('void f(const std::va_list ap) { va_end(ap); }', 'expected a mutable va_list cursor'),
            ('void f(std::va_list* ap) { va_end(ap); }', 'expected a mutable va_list cursor'),
            ('void f(void* ap) { va_end(ap); }', 'expected a mutable va_list cursor'),
            ('struct Cursor { unsigned gp, fp; void* overflow; void* save; };\n'
             'void f(Cursor* ap) { va_end(ap); }', 'expected a mutable va_list cursor'),
            ('void f(std::va_list ap) { va_arg(ap, 3); }', 'a declaration needs a type specifier'),
            ('void f(std::va_list ap) { va_arg(ap, short); }', 'outside the supported x86-64 System V'),
            ('void f(std::va_list ap) { va_arg(ap, double); }', 'outside the supported x86-64 System V'),
        )
        for source, message in invalid:
            (scope / 'invalid.cpp').write_text('#include <cstdarg>\n' + source + '\n')
            run(ycxx, 'cxx', linux, *includes, '-fsyntax-only', 'invalid.cpp', error=message)

        # Edit one unit across the register/stack boundary and compare outputs at each revision.
        edit = scope / 'edit.cpp'
        for named in (5, 7):
            parameters = ', '.join(['int'] * named)
            arguments = ', '.join(['0'] * named)
            edit.write_text('#include <cstdarg>\n'
                            f'int first({parameters}, ...) {{\n'
                            '  std::va_list ap; va_start(ap);\n'
                            '  int result = va_arg(ap, int); va_end(ap); return result;\n'
                            '}\n'
                            f'int main() {{ return first({arguments}, 59) != 59; }}\n')
            args = (ycxx, 'cxx', linux, *includes, '-c', edit.name, '-o')
            run(*args, 'fresh.o')
            run(ycxx, 'cxx', linux, '-nostdlib', 'fresh.o', 'linkage.o', '-o', 'fresh')
            run('./fresh')
            for _ in range(2):
                run(*args, 'resident.o', env=daemon)
                assert (scope / 'resident.o').read_bytes() == (scope / 'fresh.o').read_bytes()

        # Use the same daemon scope to switch targets, then switch back.
        (scope / 'target.cpp').write_text('''
#if defined(__linux__) != __has_builtin(__builtin_c23_va_start)
#error inconsistent variadic builtin availability
#endif
int target_control() { return 42; }
''')
        run(ycxx, 'cxx', linux, '-fsyntax-only', 'target.cpp', env=daemon)
        args = (ycxx, 'cxx', darwin, '-c', 'target.cpp', '-o')
        run(*args, 'darwin-fresh.o')
        run(*args, 'darwin-resident.o', env=daemon)
        assert (scope / 'darwin-resident.o').read_bytes() == (scope / 'darwin-fresh.o').read_bytes()
        run(ycxx, 'cxx', linux, *includes, '-c', edit.name, '-o', 'resident.o', env=daemon)
        assert (scope / 'resident.o').read_bytes() == (scope / 'fresh.o').read_bytes()


if __name__ == '__main__':
    main()
