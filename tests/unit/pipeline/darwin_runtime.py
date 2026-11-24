"""The libycxx runtime an arm64 macOS link includes by default (#842).

Without `-nostdlib++`, the driver appends libycxx's static archives and GCC's emulated TLS to the
operands and starts the link with libycxx's forced-undefined names, so even a program that uses
nothing from libycxx holds its allocation table (deps/libycxx/cmake/ycxx-link.cmake).
`-nostdlib++` links without them: a libycxx header's linkage marker is then undefined.
"""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    ycxx, nm = sys.argv[1:]
    env = dict(os.environ, LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='1')
    with tempfile.TemporaryDirectory(prefix='ycxx-darwin-runtime-', dir='.') as temp:
        scope = Path(temp)

        def run(*args, status=0, error=''):
            result = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=True, timeout=60)
            if result.returncode != status or error not in result.stderr:
                raise AssertionError(f'{args} exited {result.returncode}, expected {status}'
                                     f' and stderr containing {error!r}\n{result.stdout}{result.stderr}')
            return result.stdout

        (scope / 'trivial.cpp').write_text('int main() { return 3; }\n')
        run(ycxx, 'cxx', 'trivial.cpp', '-o', 'with-runtime')
        run('./with-runtime', status=3)
        assert '___ycxx_allocation_table_anchor' in run(nm, 'with-runtime')
        run(ycxx, 'cxx', '-nostdlib++', 'trivial.cpp', '-o', 'without-runtime')
        run('./without-runtime', status=3)
        assert '___ycxx_allocation_table_anchor' not in run(nm, 'without-runtime')

        run(ycxx, 'cxx', '-fcheck-policies', 'trivial.cpp', '-o', 'checked-runtime')
        run('./checked-runtime', status=3)
        assert '___ycxx_allocation_table_anchor' in run(nm, 'checked-runtime')
        run(ycxx, 'cxx', '-fcheck-policies', '-nostdlib++', 'trivial.cpp', '-o', 'checked-no-runtime')
        run('./checked-no-runtime', status=3)
        assert '___ycxx_allocation_table_anchor' not in run(nm, 'checked-no-runtime')
        (scope / 'libycxx.a').write_bytes(b'!<arch>\n')
        run(ycxx, 'cxx', '-fcheck-policies', 'trivial.cpp', 'libycxx.a', '-o', 'checked-explicit-archive',
            status=1, error='checked ownership links require explicit objects and source inputs')
        assert not (scope / 'checked-explicit-archive').exists()

        (scope / 'cstdio.cpp').write_text('#include <cstdio>\nint main() { std::puts("hi"); return 0; }\n')
        assert run(ycxx, 'cxx', 'cstdio.cpp', '-o', 'cstdio') == ''
        assert run('./cstdio') == 'hi\n'
        run(ycxx, 'cxx', '-nostdlib++', 'cstdio.cpp', '-o', 'unlinked', status=1,
            error='undefined symbol ___ycxx_linkage_static_v1')


if __name__ == '__main__':
    main()
