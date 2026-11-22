"""Check hosted propagation before and after the active union member's lifetime begins."""

import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run


def main():
    ycxx, peer, readelf = sys.argv[1:]
    sources = Path(__file__).with_suffix('').resolve()
    target = [ycxx, 'cxx', '--target=x86_64-pc-linux-gnu']
    cold = dict(os.environ, LC_ALL='C', YCXX_NO_DAEMON='1')
    with tempfile.TemporaryDirectory(prefix='ycxx-union-unwind-') as scope:
        for source, consumer in (('constructor', 'peer'), ('controls', 'controls_peer')):
            run([*target, '-c', sources / f'{source}.cpp', '-o', 'constructor.o'], scope, cold)
            metadata = run([readelf, '--wide', '--sections', '--relocs', 'constructor.o'], scope, cold)
            for name in ('.gcc_except_table', '.eh_frame', '__gxx_personality_v0', '_Unwind_Resume'):
                assert name in metadata, metadata
            assert '__cxa_begin_catch' not in metadata and '__cxa_end_catch' not in metadata, metadata
            for opt in ('-O0', '-O2'):
                run([peer, '-std=c++26', '-pedantic-errors', '-fno-lto', opt,
                     sources / f'{consumer}.cpp', 'constructor.o', '-o', 'mixed'], scope, cold)
                run(['./mixed'], scope, cold)

        original = (sources / 'constructor.cpp').read_text()
        unsupported = Path(scope) / 'unsupported.cpp'
        for source, diagnostic in (
            (original.replace('{ external_throw(); }', '{ external_throw(); external_throw(); }'),
             'unsupported unwind from a union constructor with a nontrivial active member'),
            (original.replace('void probe() { Storage object; }', 'int probe() { Storage object; return 0; }'),
             'unsupported caller of a potentially throwing union constructor'),
        ):
            unsupported.write_text(source)
            run([*target, '-c', unsupported, '-o', 'rejected.o'], scope, cold, error=diagnostic)
            assert not (Path(scope) / 'rejected.o').exists()

        pattern = original.replace('Storage() :', 'template<class T> Storage(T) :')
        unsupported.write_text(pattern.replace('Storage object;', 'Storage object(0);'))
        run([*target, '-fsyntax-only', unsupported], scope, cold,
            error='potentially throwing union construction with a nontrivial member')


if __name__ == '__main__':
    main()
