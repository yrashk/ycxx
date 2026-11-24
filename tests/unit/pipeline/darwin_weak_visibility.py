"""Explicit GNU weak functions and namespace visibility in arm64 Mach-O.

The Darwin counterpart of the weak providers in linux_inline.py (#937) and of
linux_namespace_visibility.py (#940): an explicitly weak ordinary function is a weak definition, a
function declared in a namespace with a visibility attribute takes that visibility, and a strong
definition wins over a weak one under both ycxx's linker and the system linker.
"""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


WEAK_UNITS = {
    'weak-main': 'int selected() noexcept;\nint main() { return selected(); }\n',
    'weak-provider': '[[__gnu__::__weak__]] int selected() noexcept { return 17; }\n',
    'weak-redecl': ('extern "C" __attribute__((weak, visibility("hidden"))) int hook() noexcept;\n'
                    'int hook() noexcept { return 17; }\n'),
    'strong-provider': 'int selected() noexcept { return 31; }\n',
    'duplicate-provider': 'int selected() noexcept { return 41; }\n',
}

QUALIFIED = '''\
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
'''

CALLER = '''\
namespace api { inline namespace v1 { int probe() noexcept; } }
namespace outside { int neighbor() noexcept; }

int main() {
  if (api::probe() != 17) return 2;
  return outside::neighbor() != 23;
}
'''

BLOCKS = '''\
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
'''

VARIABLES = '''\
__attribute__((visibility("hidden"))) int hidden_value = 1;
__attribute__((visibility("hidden"))) int hidden_zero;
__attribute__((visibility("hidden"))) extern const int hidden_constant;
const int hidden_constant = 3;
int plain_value = 4;
int read_values() { return hidden_value + hidden_zero + hidden_constant + plain_value; }
'''

READER = '''\
int read_values();
extern int plain_value;
int main() { return read_values() + plain_value; }
'''

# Weak definitions that unwind: one with a cleanup, one cleanup-free. Their frames must describe
# their own code, whichever definition the name resolves to.
WEAK_UNWIND = '''\
extern "C" void may_throw();
struct Guard {
  int* count;
  ~Guard() noexcept { ++*count; }
};
[[__gnu__::__weak__]] void guarded(int* count) {
  Guard guard{count};
  may_throw();
}
[[__gnu__::__weak__]] void forward() { may_throw(); }
'''

STRONG_UNWIND = '''\
extern "C" void may_throw();
extern int pending;
void forward() {
  if (pending) ++pending;
  may_throw();
}
'''

# Clang's: the throw and the handlers, which ycxx does not compile on arm64 Darwin yet (#648).
# With an argument, only the paths that do not throw: ycxx's linker does not keep Clang's
# compact unwind entries for the handlers yet (#72).
UNWIND_HOST = '''\
int pending;
extern "C" void may_throw() { if (pending) throw pending; }
void guarded(int*);
void forward();
int main(int argc, char**) {
  int count = 0;
  if (argc > 1) {
    guarded(&count);
    forward();
    return count == 1 ? 0 : 6;
  }
  pending = 7;
  try { guarded(&count); return 1; } catch (int v) { if (v != 7 || count != 1) return 2; }
  pending = 9;
  try { forward(); return 3; } catch (int v) { if (v != 9 + DELTA) return 4; }
  pending = 0;
  guarded(&count);
  if (DELTA == 0) forward();
  return count == 2 ? 0 : 5;
}
'''

WEAK, STRONG = 'weak external', 'external'
WEAK_HIDDEN, HIDDEN = 'weak private external', 'private external'


def main():
    ycxx, host_cxx, nm = sys.argv[1:]
    options = (ycxx, 'cxx', '-std=c++26')
    env = dict(os.environ, LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='1')
    with tempfile.TemporaryDirectory(prefix='ycxx-weak-visibility-', dir='.') as temp:
        scope = Path(temp)

        def run(*args, status=0, error=''):
            result = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=True, timeout=60)
            if result.returncode != status or error not in result.stderr:
                raise AssertionError(f'{args} exited {result.returncode}, expected {status}'
                                     f' and stderr containing {error!r}\n{result.stdout}{result.stderr}')
            return result.stdout

        def compile(source, obj, *flags):
            run(*options, *flags, '-c', source, '-o', obj)

        def defined(obj):
            """The binding `nm -m` reports for each symbol the object defines."""
            table = {}
            for line in run(nm, '-m', obj).splitlines():
                m = re.match(r'^[0-9a-f]+\s+\((__[A-Z]+),\S+\)\s+((?:weak )?(?:private )?(?:non-)?external)\s+(\S+)$', line)
                if m and m[1] != '__LD':
                    table[m[3]] = m[2]
            return table

        def expect(obj, expected):
            table = defined(obj)
            for name, binding in expected.items():
                assert table.get(name) == binding, (obj, name, binding, table)

        # Weak providers: the binding, then which definition each linker keeps.
        for name, contents in WEAK_UNITS.items():
            (scope / f'{name}.cpp').write_text(contents)
            compile(f'{name}.cpp', f'{name}.o')
        expect('weak-provider.o', {'__Z8selectedv': WEAK})
        expect('weak-redecl.o', {'_hook': WEAK_HIDDEN})
        expect('strong-provider.o', {'__Z8selectedv': STRONG})
        run(*options, 'weak-main.o', 'weak-provider.o', '-o', 'program')
        run('./program', status=17)
        for order in (('weak-provider.o', 'strong-provider.o'), ('strong-provider.o', 'weak-provider.o')):
            run(*options, 'weak-main.o', *order, '-o', 'program')
            run('./program', status=31)
            run(host_cxx, 'weak-main.o', *order, '-o', 'host-program')
            run('./host-program', status=31)
        run(*options, 'weak-main.o', 'strong-provider.o', 'duplicate-provider.o', '-o', 'program', status=1,
            error='duplicate')

        # Unwinding through weak definitions into Clang's handlers, alone and with a strong
        # definition that wins, under both linkers.
        (scope / 'weak-unwind.cpp').write_text(WEAK_UNWIND)
        (scope / 'strong-unwind.cpp').write_text(STRONG_UNWIND)
        (scope / 'unwind-host.cpp').write_text(UNWIND_HOST)
        compile('weak-unwind.cpp', 'weak-unwind.o')
        compile('strong-unwind.cpp', 'strong-unwind.o')
        expect('weak-unwind.o', {'__Z7guardedPi': WEAK, '__Z7forwardv': WEAK})
        (scope / 'plain-unwind.cpp').write_text(WEAK_UNWIND.replace('[[__gnu__::__weak__]] ', ''))
        compile('plain-unwind.cpp', 'plain-unwind.o')

        def frame_bytes(program):
            """The size of the program's __TEXT,__eh_frame."""
            lines = run('otool', '-l', program).splitlines()
            at = next(k for k, line in enumerate(lines) if line.split() == ['sectname', '__eh_frame'])
            return next(int(line.split()[1], 16) for line in lines[at:] if line.split()[0] == 'size')

        run(host_cxx, '-std=c++26', '-DDELTA=0', '-c', 'unwind-host.cpp', '-o', 'unwind-host.o')
        run(*options, 'unwind-host.o', 'plain-unwind.o', '-o', 'plain-program')
        run(*options, 'unwind-host.o', 'weak-unwind.o', '-o', 'program')
        assert frame_bytes('program') == frame_bytes('plain-program'), 'a weak definition lost its frame'
        for delta, objects in ((0, ('weak-unwind.o',)), (1, ('weak-unwind.o', 'strong-unwind.o')),
                               (1, ('strong-unwind.o', 'weak-unwind.o'))):
            run(host_cxx, '-std=c++26', f'-DDELTA={delta}', '-c', 'unwind-host.cpp', '-o', 'unwind-host.o')
            run(*options, 'unwind-host.o', *objects, '-o', 'program')
            run('./program', 'quiet')
            run(host_cxx, 'unwind-host.o', *objects, '-o', 'host-program')
            run('./host-program')

        # A qualified definition takes its namespace's visibility; a caller in another object
        # still reaches it, and the default one stays exported.
        (scope / 'qualified.cpp').write_text(QUALIFIED)
        (scope / 'caller.cpp').write_text(CALLER)
        compile('caller.cpp', 'caller.o')
        for visibility, binding in (('hidden', HIDDEN), ('default', STRONG)):
            obj = f'qualified-{visibility}.o'
            compile('qualified.cpp', obj, f'-DAPI_VISIBILITY="{visibility}"')
            expect(obj, {'__ZN3api2v15probeEv': binding, '__ZN7outside8neighborEv': STRONG})
            run(*options, 'caller.o', obj, '-o', 'program')
            run('./program')
            run(host_cxx, 'caller.o', obj, '-o', 'host-program')
            run('./host-program')

        # Nested namespace blocks: the innermost attribute applies, and a later block without
        # one adds nothing.
        (scope / 'blocks.cpp').write_text(BLOCKS)
        compile('blocks.cpp', 'blocks.o')
        expect('blocks.o', {
            '__ZN5outer2v11fEv': HIDDEN,
            '__ZN5outer2v11gEv': STRONG,
            '__ZN5outer5inner7visibleEv': STRONG,
            '__ZN5outer5inner4leaf6hiddenEv': HIDDEN,
        })

        # Variables with their own hidden attribute, initialized, zero and constant, and a reader in
        # another object.
        (scope / 'variables.cpp').write_text(VARIABLES)
        (scope / 'reader.cpp').write_text(READER)
        compile('variables.cpp', 'variables.o')
        compile('reader.cpp', 'reader.o')
        expect('variables.o', {'_hidden_value': HIDDEN, '_hidden_zero': HIDDEN, '_hidden_constant': HIDDEN,
                               '_plain_value': STRONG, '__Z11read_valuesv': STRONG})
        run(*options, 'reader.o', 'variables.o', '-o', 'program')
        run('./program', status=12)
        run(host_cxx, 'reader.o', 'variables.o', '-o', 'host-program')
        run('./host-program', status=12)

        # The same table as Clang's for each unit, the platform's C++ ABI.
        for source, flags in (('weak-provider.cpp', ()), ('weak-redecl.cpp', ()), ('blocks.cpp', ()), ('variables.cpp', ()),
                              ('qualified.cpp', ('-DAPI_VISIBILITY="hidden"',))):
            run(host_cxx, '-std=c++26', *flags, '-c', source, '-o', 'host.o')
            compile(source, 'ycxx.o', *flags)
            def exported(obj):
                return {name: binding for name, binding in defined(obj).items() if binding != 'non-external'}
            assert exported('ycxx.o') == exported('host.o'), (source, exported('ycxx.o'), exported('host.o'))


if __name__ == '__main__':
    main()
