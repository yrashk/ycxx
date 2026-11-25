"""Build and run Linux x86-64 freestanding C++ executables through the driver."""

import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile


def main():
    ycxx, readelf = sys.argv[1:]
    target = '--target=x86_64-pc-linux-gnu'
    darwin = '--target=arm64-apple-darwin'
    with tempfile.TemporaryDirectory(prefix='ycxx-linux-exec-', dir='.') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temp).resolve()
        (scope / '.ycxx').touch()
        base = {k: v for k, v in os.environ.items() if k not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT')}
        base.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')
        cold = dict(base, YCXX_NO_DAEMON='1')
        daemon = dict(base, YCXX_DAEMON='require')

        def run(args, status=0, error='', env=cold):
            result = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=True, timeout=30)
            if result.returncode != status or error not in result.stderr:
                raise AssertionError(f'{args} exited {result.returncode}, expected {status}'
                                     f' and stderr containing {error!r}\n{result.stdout}{result.stderr}')
            return result.stdout

        def compile(*args, env=cold, status=0, error=''):
            return run([ycxx, 'cxx', *args], env=env, status=status, error=error)

        def source(name, text):
            (scope / name).write_text(text + '\n')

        def reject(*args, error, env=cold):
            compile(target, '-nostdlib', *args, '-o', 'rejected', status=1, error=error, env=env)
            assert not (scope / 'rejected').exists()

        source('trivial.cpp', 'int main() {}')
        compile(target, '-nostdlib', 'trivial.cpp', env=dict(cold, PATH=''))
        run(['./a.out'])
        headers = run([readelf, '-h', '-l', 'a.out'])
        assert 'EXEC (Executable file)' in headers and 'X86-64' in headers
        assert 'INTERP' not in headers and 'DYNAMIC' not in headers
        compile(darwin, 'trivial.cpp', '-o', 'darwin-program')
        assert (scope / 'darwin-program').read_bytes().startswith(bytes.fromhex('cffaedfe'))

        tests = Path(__file__).resolve().parents[2]
        strlen = tests / 'own' / 'builtins' / 'strlen-runtime.run.cpp'
        compile(target, '-nostdlib', str(strlen), '-o', 'strlen-program', env=dict(cold, PATH=''))
        for args in ((), ('',), ('q',), ('', 'offset'), ('q', 'offset')):
            run(['./strlen-program', *args])

        for fixture in ('bit-cast-representation.run.cpp', 'bit-cast-discard-result.run.cpp'):
            bit_cast = tests / 'own' / 'builtins' / fixture
            compile(target, '-std=c++26', '-nostdlib', str(bit_cast), '-o', 'bit-cast-program')
            run(['./bit-cast-program'])
            run(['./bit-cast-program', 'different-input'])

        source('trap.cpp', """int main(int argc, char**) {
  return argc > 1 ? (__builtin_trap(), 91) : 37;
}""")
        compile(target, '-nostdlib', 'trap.cpp', '-o', 'trap-program', env=dict(cold, PATH=''))
        run(['./trap-program'], status=37)
        run(['./trap-program', 'trigger'], status=-signal.SIGILL)
        (scope / 'darwin-trap').mkdir()
        compile(darwin, '-c', 'trap.cpp', '-o', 'darwin-trap/trap.o')
        trap_object = (scope / 'darwin-trap/trap.o').read_bytes()
        # Little-endian Mach-O 64, arm64, MH_OBJECT.
        assert trap_object[:8] == bytes.fromhex('cffaedfe 0c000001')
        assert trap_object[12:16] == bytes.fromhex('01000000')

        source('main.cpp', """int add_bias(int);
int main(int argc, char* argv[]) {
  if (argc != 3 || argv[0] == nullptr || argv[argc] != nullptr) return 1;
  return add_bias(argv[1][0]) + argv[2][0];
}""")
        source('bias.cpp', 'static int bias = 7; int add_bias(int value) { return value + bias; }')
        compile('-nostdlib', '--target', 'x86_64-linux-gnu', 'main.cpp', 'bias.cpp', '-o', 'program')
        run(['./program', 'A', '!'], status=105)
        run(['./program', 'C', '#'], status=109)
        run(['./program'], status=1)
        image = (scope / 'program').read_bytes()
        assert not list(scope.glob('*.o'))

        source('alias.cpp', 'using Argv = char**; int start(int count, Argv) asm("main");'
                           ' int start(int count, Argv) { return count + 32; }')
        source('bad_alias.cpp', 'long start() asm("main"); long start() { return 1; }')
        compile(target, '-nostdlib', 'alias.cpp', '-o', 'alias')
        run(['./alias', 'argument'], status=34)
        bad_abi = "Linux startup requires 'int main()' or 'int main(int, char**)'"
        reject('bad_alias.cpp', error=bad_abi)
        source('wrong_params.cpp', 'int entry(int) asm("main"); int entry(int x) { return x; }')
        reject('wrong_params.cpp', error=bad_abi)
        source('bad_main.cpp', 'void main() {}')
        reject('bad_main.cpp', error="'main' must return 'int'")
        source('start.cpp', 'extern "C" void _start() { for (;;) {} }')
        compile(target, '-nostdlib', 'start.cpp', '-o', 'explicit-entry')
        compile(target, '-nostdlib', 'start.cpp', 'bad_alias.cpp', '-o', 'explicit-with-main')

        reject('bias.cpp', error="ELF entry symbol 'main' is undefined")
        reject('main.cpp', error="undefined symbol '_Z8add_biasi'")
        reject('trivial.cpp', 'main.cpp', 'bias.cpp', error="duplicate symbol 'main'")
        source('init-first.cpp', """extern "C" int __cxa_atexit(void (*)(void*), void*, void*);
extern "C" char __dso_handle;
static int order;
int append(int digit) { order = order * 10 + digit; return order; }
static const int first = append(1), second = append(2);
int current() { return order + first + second; }""")
        source('init-second.cpp', 'int append(int); static int third = append(3);')
        source('init-main.cpp', """int current();
int main(int argc, char** argv) {
  if (argc != 2 || argv[0] == nullptr || argv[argc] != nullptr || argv[1][0] != 'Q') return 1;
  if (current() == 136) return 23;
  return current() == 655 ? 29 : 2;
}""")
        compile(target, '-nostdlib', 'init-first.cpp', 'init-second.cpp', 'init-main.cpp', '-o', 'init-program')
        run(['./init-program', 'Q'], status=23)
        run(['./init-program'], status=1)
        init_image = (scope / 'init-program').read_bytes()
        for part in ('first', 'second', 'main'):
            compile(target, '-c', f'init-{part}.cpp')
        assert '.init_array' not in run([readelf, '-SW', 'init-main.o'])
        compile(target, '-nostdlib', 'init-first.o', 'init-second.o', 'init-main.o', '-o', 'init-objects')
        run(['./init-objects', 'Q'], status=23)
        assert (scope / 'init-objects').read_bytes() == init_image
        compile(target, '-nostdlib', 'init-second.o', 'init-first.o', 'init-main.o', '-o', 'init-objects')
        run(['./init-objects', 'Q'], status=29)
        compile(target, '-nostdlib', 'init-first.cpp', 'init-second.o', 'init-main.o', '-o', 'init-mixed')
        run(['./init-mixed', 'Q'], status=23)
        compile(target, '-nostdlib', 'init-second.cpp', 'init-first.o', 'init-main.o', '-o', 'init-mixed')
        run(['./init-mixed', 'Q'], status=29)
        custom_init = "a custom '_start' with a nonempty ELF .init_array is unsupported"
        reject('start.cpp', 'init-first.cpp', error=custom_init)
        reject('init-first.o', 'start.cpp', error=custom_init)
        compile(darwin, '-c', 'init-first.cpp', '-o', 'darwin-dynamic.o')
        assert b'__mod_init_func' in (scope / 'darwin-dynamic.o').read_bytes()

        reference_fixtures = tests / 'own' / 'dynamic-init'
        for fixture in ('constant', 'ordered', 'constinit', 'constexpr-address', 'extern'):
            path = str(reference_fixtures / f'reference-{fixture}.run.cpp')
            compile(target, '-c', path, '-o', 'reference.o')
            compile(target, '-nostdlib', 'reference.o', '-o', 'reference')
            run(['./reference'])
            relocations = run([readelf, '-Wr', 'reference.o'])
            if fixture == 'constant':
                data = relocations.split("'.rela.data'", 1)[1].split('\n\n', 1)[0]
                assert sum('R_X86_64_64' in line and line.rstrip().endswith('targetE + 0')
                           for line in data.splitlines()) == 2
            if fixture == 'constinit':
                assert "'.rela.init_array'" not in relocations
            if fixture == 'ordered':
                assert "'.rela.init_array'" in relocations
                assert 'R_X86_64_64' in relocations and '__GLOBAL__sub_I_' in relocations
        bindings = tests / 'own' / 'structured-bindings'
        for fixture in ('ordinary', 'qualifiers', 'dispatch', 'lifetime'):
            compile(target, '-nostdlib', bindings / f'{fixture}.run.cpp', '-o', 'bindings')
            run(['./bindings'])
        compile(target, '-fsyntax-only', bindings / 'tuple-selection.reject.cpp', status=1,
                error="tuple-like structured binding for type 'Crate' is not supported yet")
        compile(target, '-fsyntax-only', bindings / 'conditional-temporary.accept.cpp', status=1,
                error='structured binding initializer needs unsupported temporary lifetime extension')
        temporary = str(tests / 'draft' / 'basic' / 'class.temporary-3.accept.cpp')
        compile(target, '-fsyntax-only', temporary)
        compile(target, '-c', temporary, '-o', 'reference-temporary.o', status=1,
                error='a materialized temporary in a namespace-scope reference initializer')
        assert not (scope / 'reference-temporary.o').exists()
        source('reference-declaration-compat.cpp', """static constexpr int storage = 3;
struct C { static const int& member; };
const int& C::member = storage;
template<class T> const int& variable = storage;
int read() { return variable<int>; }""")
        compile(target, '-fsyntax-only', 'reference-declaration-compat.cpp')

        source('teardown.cpp', """struct Hook { int value; ~Hook(); };
Hook::~Hook() { value = 0; }
int seed() { return 7; }
int earlier = seed();
constinit Hook hook{17};
int main() { return earlier + hook.value; }""")
        teardown_error = 'teardown.cpp:5:16: error: namespace-scope destruction is not supported on x86-64 Linux'
        compile(target, '-fsyntax-only', 'teardown.cpp')
        reject('teardown.cpp', error=teardown_error)
        compile(target, '-c', 'teardown.cpp', status=1, error=teardown_error)
        assert not (scope / 'teardown.o').exists()
        compile(target, 'trivial.cpp', status=1, error='hosted Linux linking is not supported')
        compile(target, '-c', 'main.cpp')
        compile(target, '-c', 'bias.cpp')
        compile(target, 'main.o', 'bias.o', status=1, error='hosted Linux linking is not supported')
        compile(target, '-nostdlib', 'main.o', 'bias.o', '-o', 'object-program', env=dict(cold, PATH=''))
        run(['./object-program', 'A', '!'], status=105)
        compile(target, '-nostdlib', 'bias.o', 'main.cpp', '-o', 'mixed-program')
        run(['./mixed-program', 'C', '#'], status=109)
        compile(target, '-nostdlib', 'main.o', 'bias.cpp', '-o', 'mixed-program')
        run(['./mixed-program', 'A', '!'], status=105)
        reject('bias.o', 'bad_alias.cpp', error=bad_abi)
        compile(target, '-c', 'start.cpp')
        compile(target, '-nostdlib', 'start.o', '-o', 'explicit-object-entry')
        compile(target, '-nostdlib', 'start.o', 'bad_alias.cpp', '-o', 'explicit-object-with-main')

        source('static_locals.cpp', """namespace outer::inline inner {
struct Counter { int value; };
int tick() { static Counter count = {10}; return ++count.value; }
}
int main() { static int base = 20; return base + outer::tick() + outer::tick(); }""")
        compile(target, '-c', 'static_locals.cpp')
        symbols = run([readelf, '-Ws', 'static_locals.o'])
        assert ' _ZZN5outer5inner4tickEvE5count\n' in symbols
        assert ' _ZZ4mainE4base\n' in symbols
        compile(target, '-nostdlib', 'static_locals.o', '-o', 'static-locals')
        run(['./static-locals'], status=43)

        source('guarded_static.cpp', 'int remember(int value) { static int first = value; return first; }\n'
                                    'int main() { return remember(17) + remember(99); }')
        compile(target, '-c', 'guarded_static.cpp')
        symbols = run([readelf, '-Ws', 'guarded_static.o'])
        assert ' _ZZ8rememberiE5first\n' in symbols
        assert ' _ZGVZ8rememberiE5first\n' in symbols
        reject('guarded_static.o', error="undefined symbol '__cxa_guard_acquire'")

        compile(darwin, '-c', 'trivial.cpp', '-o', 'darwin.o')
        compile(darwin, 'darwin.o', '-o', 'darwin-object-program')
        assert (scope / 'darwin-object-program').read_bytes().startswith(bytes.fromhex('cffaedfe'))
        reject('main.cpp', 'darwin.o', error='darwin.o: expected a little-endian ELF64 object')
        compile(darwin, 'trivial.cpp', 'main.o', status=1, error='main.o: not a 64-bit Mach-O file')
        (scope / 'malformed.o').write_bytes(b'\x7fELF')
        reject('malformed.o', error='malformed.o: truncated ELF header')
        reject('missing.o', error="cannot read 'missing.o'")
        reject('bad_main.cpp', 'malformed.o', error="'main' must return 'int'")
        compile(darwin, '-nostdlib', 'trivial.cpp', status=1, error="unsupported option '-nostdlib'")

        source('host-default.cpp', """#if !defined(__linux__) || !defined(__x86_64__) || defined(__APPLE__)
#error expected the Linux x86-64 host target
#endif
int main() { return 27; }""")
        compile('host-default.cpp', '-o', 'host-hosted', status=1, error='hosted Linux linking is not supported')
        assert not (scope / 'host-hosted').exists()
        preprocessed = compile('-E', 'host-default.cpp')
        assert preprocessed == compile(target, '-E', 'host-default.cpp')
        compile('-c', 'host-default.cpp', '-o', 'host-default.o')
        headers = run([readelf, '-h', 'host-default.o'])
        assert 'REL (Relocatable file)' in headers and 'X86-64' in headers
        object_image = (scope / 'host-default.o').read_bytes()
        compile('-nostdlib', 'host-default.o', '-o', 'host-default')
        run(['./host-default'], status=27)
        headers = run([readelf, '-h', 'host-default'])
        assert 'EXEC (Executable file)' in headers and 'X86-64' in headers
        default_image = (scope / 'host-default').read_bytes()

        try:
            assert compile('-E', 'host-default.cpp', env=daemon) == preprocessed
            compile('-c', 'host-default.cpp', '-o', 'host-default.o', env=daemon)
            assert (scope / 'host-default.o').read_bytes() == object_image
            compile('-nostdlib', 'host-default.o', '-o', 'host-default', env=daemon)
            assert (scope / 'host-default').read_bytes() == default_image
            run(['./host-default'], status=27)
            for _ in range(2):
                compile(target, '-nostdlib', 'main.cpp', 'bias.cpp', '-o', 'program', env=daemon)
                run(['./program', 'A', '!'], status=105)
                assert (scope / 'program').read_bytes() == image
            compile(target, '-nostdlib', 'main.o', 'bias.o', '-o', 'object-program', env=daemon)
            run(['./object-program', 'A', '!'], status=105)
            source('bias.cpp', 'int add_bias(int value) { return value + 8; }')
            compile(target, '-c', 'bias.cpp')
            compile(target, '-nostdlib', 'main.o', 'bias.o', '-o', 'object-program', env=daemon)
            run(['./object-program', 'A', '!'], status=106)
            reject('bad_alias.cpp', error=bad_abi, env=daemon)
            compile(target, '-nostdlib', 'init-first.cpp', 'init-second.cpp', 'init-main.cpp', '-o', 'init-program', env=daemon)
            run(['./init-program', 'Q'], status=23)
            assert (scope / 'init-program').read_bytes() == init_image
            reject('teardown.cpp', error=teardown_error, env=daemon)
        finally:
            run([ycxx, 'ctl', 'shutdown'], env=base)


if __name__ == '__main__':
    main()
