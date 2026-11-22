"""Exercise one automatic destructor on both paths of a hosted Linux call."""

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
    with tempfile.TemporaryDirectory(prefix='ycxx-local-unwind-') as scope:
        run([*target, '-c', sources / 'checked.cpp', '-o', 'checked.o'], scope, cold)
        metadata = run([readelf, '--wide', '--sections', '--relocs', 'checked.o'], scope, cold)
        for name in ('.gcc_except_table', '.eh_frame', '__gxx_personality_v0', '_Unwind_Resume'):
            assert name in metadata, metadata
        assert '__cxa_begin_catch' not in metadata and '__cxa_end_catch' not in metadata, metadata
        for opt in ('-O0', '-O2'):
            run([peer, '-std=c++26', '-pedantic-errors', '-fno-lto', opt,
                 sources / 'peer.cpp', 'checked.o', '-o', 'mixed'], scope, cold)
            run(['./mixed'], scope, cold)
        nonthrowing = Path(scope) / 'nonthrowing.cpp'
        nonthrowing.write_text((sources / 'checked.cpp').read_text().replace('void may_throw();', 'void may_throw() noexcept;'))
        run([*target, '-c', nonthrowing, '-o', 'nonthrowing.o'], scope, cold)
        assert '.gcc_except_table' not in run([readelf, '--sections', 'nonthrowing.o'], scope, cold)
        for case in ('two_live_locals', 'throwing_destructor', 'conditional_call'):
            source = sources / f'{case}.cpp'
            run([*target, '-fsyntax-only', source], scope, cold)
            run([*target, '-c', source, '-o', 'rejected.o'], scope, cold, error='unsupported local unwind cleanup')
            assert not (Path(scope) / 'rejected.o').exists()


if __name__ == '__main__':
    main()
