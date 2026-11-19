"""Build and run Linux x86-64 freestanding C++ executables through the driver."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    ycxx, readelf = sys.argv[1:]
    target = '--target=x86_64-pc-linux-gnu'
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
        compile('trivial.cpp', '-o', 'darwin-program')
        assert (scope / 'darwin-program').read_bytes().startswith(bytes.fromhex('cffaedfe'))

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

        compile('-c', 'trivial.cpp', '-o', 'darwin.o')
        compile('darwin.o', '-o', 'darwin-object-program')
        assert (scope / 'darwin-object-program').read_bytes().startswith(bytes.fromhex('cffaedfe'))
        reject('main.cpp', 'darwin.o', error='darwin.o: expected a little-endian ELF64 object')
        compile('trivial.cpp', 'main.o', status=1, error='main.o: not a 64-bit Mach-O file')
        (scope / 'malformed.o').write_bytes(b'\x7fELF')
        reject('malformed.o', error='malformed.o: truncated ELF header')
        reject('missing.o', error="cannot read 'missing.o'")
        reject('bad_main.cpp', 'malformed.o', error="'main' must return 'int'")
        compile('-nostdlib', 'trivial.cpp', status=1, error="unsupported option '-nostdlib'")

        try:
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
        finally:
            run([ycxx, 'ctl', 'shutdown'], env=base)


if __name__ == '__main__':
    main()
