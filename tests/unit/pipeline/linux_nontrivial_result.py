"""Check ordinary System V AMD64 calls with nontrivial class results."""

import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run


def main():
    ycxx, host_cxx = sys.argv[1:]
    sources = Path(__file__).resolve().parent / 'linux_nontrivial_result'
    target = [ycxx, 'cxx', '--target=x86_64-pc-linux-gnu']
    cold = dict(os.environ, LC_ALL='C', YCXX_NO_DAEMON='1')
    with tempfile.TemporaryDirectory(prefix='ycxx-result-') as scope:
        run([*target, '-nostdlib', sources / 'native.cpp', '-o', 'native'], scope, cold)
        run(['./native'], scope, cold)
        for role in ('client', 'provider'):
            run([*target, '-c', sources / f'{role}.cpp', '-o', f'{role}.o'], scope, cold)
        run([*target, '-nostdlib', 'client.o', 'provider.o', '-o', 'native-split'], scope, cold)
        run(['./native-split'], scope, cold)

        for opt in ('-O0', '-O2'):
            peer = [host_cxx, '-std=c++26', '-pedantic-errors', opt, '-fno-lto']
            for role, native in (('client', 'provider'), ('provider', 'client')):
                run([*peer, sources / f'{role}.cpp', f'{native}.o', '-o', 'mixed'], scope, cold)
                run(['./mixed'], scope, cold)
        run([host_cxx, sources / 'echo_client.cpp', sources / 'echo_witness.S',
             'provider.o', '-o', 'echo'], scope, cold)
        run(['./echo'], scope, cold)

        uncertain = Path(scope) / 'uncertain.cpp'
        uncertain.write_text('#pragma pack(push, 1)\n'
                             'struct Packed { char tag; int payload; };\n'
                             '#pragma pack(pop)\n'
                             'struct Handle { Packed value; ~Handle() {} };\n'
                             'Handle make() { return Handle{{1, 42}}; }\n')
        run([*target, '-fsyntax-only', uncertain], scope, cold)
        run([*target, '-c', uncertain], scope, cold, error='unsupported aggregate result layout')


if __name__ == '__main__':
    main()
