"""Link, load, and consume a native Linux x86-64 shared library (#391)."""

import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


PROVIDER = '''
static int adjustment = BIAS;
static int calls;
static int add_adjustment(int value) { return value + adjustment; }

extern "C" int score(int value) { return add_adjustment(value); }
extern "C" int adjust(int delta) { return adjustment += delta; }

struct Host {
    void* context;
    int (*emit)(void*, const char*);
};

extern "C" int plan(const Host* host) {
    return host->emit(host->context, "plugin output") + calls++;
}
'''

LOAD_PLUGIN = '''
import ctypes as c
import sys

emit_type = c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_char_p)

class Host(c.Structure):
    _fields_ = [('context', c.c_void_p), ('emit', emit_type)]

calls = []

@emit_type
def emit(context, text):
    calls.append((context, text))
    return 23

library = c.CDLL(sys.argv[1])
library.plan.argtypes = (c.POINTER(Host),)
assert library.plan(c.byref(Host(42, emit))) == 23
assert library.plan(c.byref(Host(42, emit))) == 24
assert calls == [(42, b'plugin output'), (42, b'plugin output')]
expected = int(sys.argv[2])
assert library.score(35) == expected
assert library.adjust(3) == expected - 35 + 3
assert library.score(35) == expected + 3
'''


def main():
    ycxx, cc, readelf = sys.argv[1:]
    target = '--target=x86_64-pc-linux-gnu'
    soname = 'libycxx-shared-test.so.1'

    with tempfile.TemporaryDirectory(prefix='ycxx-shared-output-', dir='.') as temp, \
            tempfile.TemporaryDirectory(prefix='ys-') as runtime:
        scope = Path(temp).resolve()
        (scope / '.ycxx').touch()
        for directory in ('staging', 'installed'):
            (scope / directory).mkdir()
        base = {key: value for key, value in os.environ.items() if key not in
                ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT', 'LD_LIBRARY_PATH', 'LD_PRELOAD', 'LD_AUDIT')}
        base.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')
        loader_env = dict(base, LD_LIBRARY_PATH=str(scope / 'installed'))

        def run(*args, status=0, error='', env=base):
            result = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=True, timeout=45)
            if result.returncode != status or error not in result.stderr:
                raise AssertionError(f'{args} exited {result.returncode}, expected {status} and stderr containing '
                                     f'{error!r}\n{result.stdout}{result.stderr}')
            return result

        def cold(*args, **kwargs):
            return run(ycxx, 'cxx', '--no-daemon', *args, **kwargs)

        def source(name, text):
            (scope / name).write_text(text + '\n')

        def plugin(bias):
            source('provider.cpp', PROVIDER.replace('BIAS', str(bias)))

        def load(path, score):
            run(sys.executable, '-c', LOAD_PLUGIN, str(scope / path), str(score))

        shared = (target, '-shared', '-nostdlib')
        plugin(7)
        staged = 'staging/provider.so'
        args = (*shared, '-fPIC', f'-Wl,-soname,{soname}', 'provider.cpp', '-o', staged)

        # PATH is empty to prove that ycxx creates the complete library itself.
        cold(*args, env=dict(base, PATH=''))
        image = (scope / staged).read_bytes()
        details = run(readelf, '-W', '-h', '-l', '-S', '-d', '--dyn-syms', staged).stdout
        assert 'DYN (Shared object file)' in details and 'X86-64' in details, details
        assert 'INTERP' not in details and '(NEEDED)' not in details and '(TEXTREL)' not in details, details
        assert all(section in details for section in ('.dynstr', '.dynsym', '.dynamic')), details
        assert re.search(rf'\(SONAME\).*\[{re.escape(soname)}\]', details), details
        exports = set(re.findall(r'\bFUNC\s+GLOBAL\s+DEFAULT\s+\d+\s+(\w+)\s*$', details, re.M))
        assert exports == {'score', 'adjust', 'plan'}, details
        load(staged, 42)

        # Both a relocatable input and the driver's default SONAME are usable.
        cold(target, '-fPIC', '-c', 'provider.cpp', '-o', 'provider.o')
        cold(*shared, 'provider.o', '-o', 'staging/default.so')
        summary = run(readelf, '-dW', 'staging/default.so').stdout
        assert re.search(r'\(SONAME\).*\[default\.so\]', summary), summary
        load('staging/default.so', 42)

        # A hidden declaration changes the effective visibility of a definition in another object.
        source('hide.s', '.globl score\n.hidden score\n.section .note.GNU-stack,"",@progbits')
        run(cc, '-c', 'hide.s', '-o', 'hide.o')
        cold(*shared, 'provider.o', 'hide.o', '-o', 'staging/hidden.so')
        hidden = run(readelf, '--dyn-syms', '-W', 'staging/hidden.so').stdout
        assert not re.search(r'\bscore\s*$', hidden, re.M), hidden

        source('system.c', 'extern int score(int); int main(void) { return score(35); }')
        run(cc, 'system.c', staged, '-o', 'system-client')
        system_headers = run(readelf, '-lW', 'system-client').stdout
        interpreter = re.search(r'\[Requesting program interpreter:\s*(/[^\]]+)\]', system_headers)
        assert interpreter, system_headers
        source('native.cpp', 'extern "C" int score(int); int main() { return score(35); }')
        cold(target, '-nostdlib', f'--dynamic-linker={interpreter.group(1)}', 'native.cpp', staged,
             '-o', 'native-client')

        # The runtime loader resolves the SONAME independently of the original link-time path.
        installed = f'installed/{soname}'
        (scope / staged).rename(scope / installed)
        for client in ('system-client', 'native-client'):
            run(f'./{client}', status=42, env=loader_env)

        def reject(*options, error):
            cold(*options, '-o', 'rejected.so', status=1, error=error)
            assert not (scope / 'rejected.so').exists()

        reject(*shared, 'provider.cpp', error="requires '-fPIC' or '-fpic'")
        reject(*shared, '-fPIC', '-Wl,-soname,dir/wrong.so', 'provider.cpp', error='SONAME')
        reject(*shared, 'hide.o', error="undefined hidden symbol 'score'")
        source('unresolved.cpp', 'extern "C" int absent(); extern "C" int f() { return absent(); }')
        reject(*shared, '-fPIC', 'unresolved.cpp', error='absent')
        source('absolute.cpp', 'static int value; static int* volatile address = &value;\n'
                               'extern "C" int f() { return *address; }')
        reject(*shared, '-fPIC', 'absolute.cpp', error='relocation')

        def traced():
            result = run(ycxx, 'cxx', '--daemon=require', '--trace-engine=json', *args)
            return [json.loads(line) for line in result.stderr.splitlines() if line.strip()]

        def event(trace, kind, outcome):
            assert any(item.get('kind') == kind and item.get('outcome') == outcome for item in trace), trace

        try:
            event(traced(), 'Link', 'executed')
            assert (scope / staged).read_bytes() == image
            event(traced(), 'Build', 'reused')
            plugin(8)
            event(traced(), 'Link', 'executed')
            assert (scope / staged).read_bytes() != image
            load(staged, 43)
        finally:
            run(ycxx, 'ctl', 'shutdown')


if __name__ == '__main__':
    main()
