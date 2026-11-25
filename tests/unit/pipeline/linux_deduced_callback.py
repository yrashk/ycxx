"""Link and run the late template callback fixtures on x86-64 ELF."""

import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run


def main():
    ycxx, host_cxx, readelf = sys.argv[1:]
    fixtures = Path(__file__).resolve().parents[2] / 'own' / 'templates'
    env = {key: value for key, value in os.environ.items() if key not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', SDKROOT='', YCXX_NO_DAEMON='1', YCXX_THREADS='1')
    compiler = [ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26', '-nostdlib']

    with tempfile.TemporaryDirectory(prefix='ycxx-deduced-callback-') as temp:
        scope = Path(temp)
        (scope / '.ycxx').touch()

        def build_and_run(name, *peers):
            source = fixtures / f'{name}.run.cpp'
            obj = f'{name}.o'
            run([*compiler, '-c', source, '-o', obj], scope, env)
            run([*compiler, obj, *peers, '-o', name], scope, env)
            run([f'./{name}'], scope, env)
            return obj

        # The type-only case previously linked against an undefined CallbackSource<int>::evaluate.
        build_and_run('deduced-callback-type-only')
        live = build_and_run('deduced-callback-live')
        table = run([readelf, '-Ws', live], scope, env)
        defined = {fields[7]: fields[3] for fields in map(str.split, table.splitlines())
                   if len(fields) >= 8 and fields[6].isdecimal()}
        assert '_ZN16DeferredCallbackIiE8callbackE' in defined, table
        assert defined.get('_ZN14CallbackSourceIiE8evaluateEv') == 'FUNC', table

        peer = 'explicit-callback-storage-peer.o'
        run([host_cxx, '-std=c++26', '-pedantic-errors', '-fno-pie', '-c',
             fixtures / 'explicit-callback-storage-peer.cpp', '-o', peer], scope, env)
        build_and_run('explicit-callback-storage', peer)


if __name__ == '__main__':
    main()
