"""Run the builtin line fixture without a hosted Linux runtime."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from linux_compile import run


def check_location_dependent_immediate(compiler, scope, env):
    source = scope / 'immediate.cpp'
    # GCC and Clang accept these defaults. ycxx currently diagnoses the immediate invocation.
    for expression, value in (('id(__builtin_LINE())', 4), ('id(__builtin_LINE()) + 1', 5)):
        source.write_text('// #327 location-dependent immediate invocation\n'
                          'consteval unsigned id(unsigned n) { return n; }\n'
                          f'constexpr unsigned f(unsigned n = {expression}) {{ return n; }}\n'
                          f'static_assert(f() == {value});\n')
        result = subprocess.run([*compiler, '-fsyntax-only', source.name], cwd=scope, env=env,
                                capture_output=True, text=True, timeout=30)
        error = ('immediate.cpp:3:35: error: an immediate invocation that observes the call-site line '
                 'of a default argument is not supported yet')
        assert result.returncode == 1 and error in result.stderr.splitlines(), (expression, result)


def main():
    ycxx, source = sys.argv[1:]
    compiler = [ycxx, 'cxx', '--no-daemon', '--target=x86_64-pc-linux-gnu', '-std=c++26']
    env = dict(os.environ, LC_ALL='C')
    with tempfile.TemporaryDirectory(prefix='ycxx-builtin-line-') as temp:
        scope = Path(temp)
        run([*compiler, '-nostdlib', '-DEXPECT_CLANG_BUILTIN_LINE', source, '-o', 'program'], scope, env)
        run(['./program'], scope, env)
        check_location_dependent_immediate(compiler, scope, env)


if __name__ == '__main__':
    main()
