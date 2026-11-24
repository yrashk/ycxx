import os
import sys
from unittest.mock import patch

from runtime_dir import runtime_root


def main():
    with patch.dict(os.environ, {'TMPDIR': '/alternate'}):
        assert runtime_root('yt') == '/alternate'
    with patch.dict(os.environ, clear=True):
        assert runtime_root('yt') == '/tmp'

    capacity = 108 if sys.platform == 'linux' else 104
    for prefix in ('yt', 'yp', 'yd', 'yccomdat-rt-'):
        # Leave space for the private directory and the 38-byte socket basename, plus the NUL.
        budget = capacity - len(prefix) - 8 - 38 - 2 - 1
        root = '/' + 'a' * (budget - 1)
        with patch.dict(os.environ, {'TMPDIR': root}):
            assert runtime_root(prefix) == root
        with patch.dict(os.environ, {'TMPDIR': root + 'a'}):
            assert runtime_root(prefix) == '/tmp'
        with patch.dict(os.environ, {'TMPDIR': root[:-1] + 'é'}):
            assert runtime_root(prefix) == '/tmp'


if __name__ == '__main__':
    main()
