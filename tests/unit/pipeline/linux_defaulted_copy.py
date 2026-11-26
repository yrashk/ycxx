"""Check first-declaration defaulted copy construction and the native by-value ABI."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    ycxx, host = sys.argv[1:]
    fixture = Path(__file__).resolve().parent / 'defaulted-copy'
    linux = '--target=x86_64-linux-gnu'
    darwin = '--target=arm64-apple-darwin'
    with tempfile.TemporaryDirectory(prefix='ycxx-copy-') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        for source in fixture.iterdir():
            shutil.copyfile(source, scope / source.name)
        base = {k: v for k, v in os.environ.items() if k not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT')}
        base.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')
        cold = dict(base, YCXX_NO_DAEMON='1')
        daemon = dict(base, YCXX_DAEMON='require')

        def run(*args, env=cold, status=0, error=None):
            res = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=True, timeout=30)
            assert res.returncode == status, (args, res.returncode, res.stdout, res.stderr)
            if error is not None:
                assert error in res.stderr, (args, res.stderr)
            return res.stderr

        def compile(*args, **kwargs):
            return run(ycxx, 'cxx', *args, **kwargs)

        for source in ('scalar-copy.cpp', 'private-copy.cpp', 'const-reference-copy.cpp',
                       'member-copy-friend.cpp', 'nontrivial-member-copy.cpp'):
            compile(linux, '-nostdlib', source, '-o', 'native')
            run('./native')
            compile(darwin, '-c', source, '-o', 'darwin.o')

        for source, lines in (('copy-only-invalid.cpp', (7, 11)), ('private-copy-invalid.cpp', (4,))):
            stderr = compile(linux, '-fsyntax-only', source, status=1)
            for line in lines:
                assert f'{source}:{line}:' in stderr, stderr

        compile(linux, '-c', 'inaccessible-member-copy-unused.cpp', '-o', 'unused.o')
        stderr = compile(linux, '-fsyntax-only', 'inaccessible-member-copy-invalid.cpp', status=1,
                         error='deleted constructor')
        assert 'inaccessible-member-copy-invalid.cpp:4:' in stderr, stderr

        # [class.copy.ctor]/10: an rvalue reference member of the leaf deletes the enclosing copy.
        dependency = 'struct Leaf { %s; };\nstruct Wrapper { Leaf leaf; Wrapper(const Wrapper&) = default; };\n'
        copy = 'void copy(const Wrapper& source) { Wrapper destination(source); }\n'
        (scope / 'implicit-leaf.cpp').write_text((dependency % 'int&& ref') + copy)
        compile(linux, '-fsyntax-only', 'implicit-leaf.cpp', status=1, error='deleted constructor')
        # [class.copy.ctor]/11: a volatile scalar does not make a copy nontrivial.
        (scope / 'implicit-leaf.cpp').write_text((dependency % 'volatile int value') + copy)
        compile(linux, '-fsyntax-only', 'implicit-leaf.cpp')

        (scope / 'repeated-leaf.cpp').write_text('''
struct Leaf { int value; };
struct Pair { Leaf first, second; };
struct Quad { Pair first, second; };
struct Wrapper {
    Quad first, second;
    Wrapper(const Wrapper&) = default;
};
void copy(const Wrapper& source) { Wrapper destination(source); }
''')
        compile(linux, '-fsyntax-only', 'repeated-leaf.cpp')

        # It is trivial for calls on both targets. The x86-64 back end does not lower a nested
        # record parameter yet, whatever its members.
        (scope / 'volatile-abi.cpp').write_text(
            (dependency % 'volatile int value') + 'int read(Wrapper object) { return object.leaf.value; }\n')
        compile(darwin, '-c', 'volatile-abi.cpp', '-o', 'darwin.o')
        compile(linux, '-fsyntax-only', 'volatile-abi.cpp')
        compile(linux, '-c', 'volatile-abi.cpp', status=1, error='unsupported aggregate parameter layout')

        for name in ('copy-abi', 'volatile-abi'):
            for part in ('main', 'peer'):
                compile(linux, '-c', f'{name}-{part}.cpp', '-o', f'copy-{part}.o')
            compile(linux, '-nostdlib', 'copy-main.o', 'copy-peer.o', '-o', 'native')
            run('./native')
            for level in ('-O0', '-O2'):
                for part in ('main', 'peer'):
                    run(host, '-std=c++26', level, '-fPIE', '-c', f'{name}-{part}.cpp', '-o', f'host-{part}.o')
                for left, right in (('copy-main.o', 'host-peer.o'), ('host-main.o', 'copy-peer.o')):
                    run(host, '-pie', left, right, '-o', 'mixed')
                    run('./mixed')

        # Keep the constructor head and the source path while changing the definition.
        edit = scope / 'edit.cpp'
        head = ('struct Edit { int value; Edit(int input) : value(input) {}\n'
                '  constexpr Edit(const Edit& source) noexcept ')
        tail = '\n};\nint main() { Edit original(7); Edit copy(original); return copy.value; }\n'
        for body, value in (('= default;', 7), ('{ value = source.value + 10; }', 17), ('= default;', 7)):
            edit.write_text(head + body + tail)
            compile(linux, '-nostdlib', edit.name, '-o', 'fresh')
            compile(linux, '-nostdlib', edit.name, '-o', 'resident', env=daemon)
            assert (scope / 'fresh').read_bytes() == (scope / 'resident').read_bytes()
            run('./resident', status=value)

        # A user-provided copy constructor makes Edit non-trivial for calls: it then passes by
        # address and returns through the hidden result pointer (#641), so the object changes.
        signature = '\n};\nEdit round_trip(Edit input) { return input; }\n'
        objects = []
        for body in ('= default;', '{ value = source.value; }', '= default;'):
            edit.write_text(head + body + signature)
            args = (linux, '-c', edit.name, '-o')
            compile(*args, 'fresh.o')
            compile(*args, 'resident.o', env=daemon)
            assert (scope / 'fresh.o').read_bytes() == (scope / 'resident.o').read_bytes()
            objects.append((scope / 'resident.o').read_bytes())
        assert objects[0] == objects[2] and objects[0] != objects[1]


if __name__ == '__main__':
    main()
