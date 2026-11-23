"""Inspect namespace visibility on qualified definitions in Linux x86-64 ELF."""

import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run


def check_functions(readelf, scope, env, obj, expected):
    table = run([readelf, '-Ws', obj], scope, env)
    for name, visibility in expected.items():
        entry, = (line.split() for line in table.splitlines() if line.split()[-1:] == [name])
        assert entry[3] == 'FUNC' and entry[5] == visibility and entry[6].isdecimal(), (
            obj, name, visibility, entry)


def check_namespace_visibility(scope, libycxx, readelf, env, compile, link):
    (scope / 'qualified.cpp').write_text('''\
namespace [[__gnu__::__visibility__(API_VISIBILITY)]] api {
inline namespace v1 {
int probe() noexcept;
}
}
namespace outside {
int neighbor() noexcept;
}

int api::probe() noexcept { return 17; }
int outside::neighbor() noexcept { return 23; }
''')

    includes = ('-isystem', libycxx / 'include')
    provider = libycxx / 'src/runtime/debugging/is_debugger_present.cpp'
    for mode, shared, visibility in (('static', '0', 'HIDDEN'), ('shared', '1', 'DEFAULT')):
        real = f'debugger-{mode}.o'
        compile(provider, real, *includes, f'-DYCXX_SHARED={shared}')
        check_functions(readelf, scope, env, real, {'_ZNSt4__y119is_debugger_presentEv': visibility})

        reduced = f'qualified-{mode}.o'
        compile('qualified.cpp', reduced, f'-DAPI_VISIBILITY="{visibility.lower()}"')
        check_functions(readelf, scope, env, reduced,
                        {'_ZN3api2v15probeEv': visibility, '_ZN7outside8neighborEv': 'DEFAULT'})

    # Reuse the exact pinned default library and call both reduced symbols from a separate unit.
    for source, obj in (('src/freestanding/pal/debugger_present.cpp', 'debugger-pal.o'),
                        ('src/runtime/linkage/static.cpp', 'debugger-marker.o')):
        compile(libycxx / source, obj, *includes, '-DYCXX_SHARED=0')
    (scope / 'caller.cpp').write_text('''\
#include <debugging>

namespace api { inline namespace v1 { int probe() noexcept; } }
namespace outside { int neighbor() noexcept; }

int main() {
  if (std::is_debugger_present()) return 1;
  if (api::probe() != 17) return 2;
  return outside::neighbor() != 23;
}
''')
    compile('caller.cpp', 'caller.o', *includes, '-DYCXX_SHARED=0')
    link('caller.o', 'debugger-static.o', 'debugger-pal.o', 'debugger-marker.o', 'qualified-static.o')
    run(['./visibility'], scope, env)


def check_namespace_blocks(scope, readelf, env, compile):
    (scope / 'blocks.cpp').write_text('''\
namespace [[__gnu__::__visibility__("hidden")]] outer {
inline namespace v1 {
int f() noexcept;
}
namespace [[__gnu__::__visibility__("default")]] inner {
int visible() noexcept;
namespace [[__gnu__::__visibility__("hidden")]] leaf {
int hidden() noexcept;
}
}
}

int outer::f() noexcept { return 17; }
int outer::inner::visible() noexcept { return 19; }
int outer::inner::leaf::hidden() noexcept { return 23; }

namespace outer {
inline namespace v1 {
int g() noexcept;
}
}
int outer::g() noexcept { return 29; }
''')
    compile('blocks.cpp', 'blocks.o')
    check_functions(readelf, scope, env, 'blocks.o', {
        '_ZN5outer2v11fEv': 'HIDDEN',
        '_ZN5outer2v11gEv': 'DEFAULT',
        '_ZN5outer5inner7visibleEv': 'DEFAULT',
        '_ZN5outer5inner4leaf6hiddenEv': 'HIDDEN',
    })


def main():
    ycxx, readelf, libycxx = sys.argv[1:]
    options = (ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26')
    env = dict(os.environ, LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='1')
    with tempfile.TemporaryDirectory(prefix='ycxx-namespace-visibility-') as temp:
        scope = Path(temp)

        def compile(source, obj, *flags):
            run([*options, *flags, '-c', source, '-o', obj], scope, env)

        def link(*objects):
            run([*options, '-nostdlib', *objects, '-o', 'visibility'], scope, env)

        check_namespace_visibility(scope, Path(libycxx), readelf, env, compile, link)
        check_namespace_blocks(scope, readelf, env, compile)


if __name__ == '__main__':
    main()
