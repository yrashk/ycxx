"""Select a temporary root that keeps ycxx's Unix socket path short enough."""

import os
import sys


def runtime_root(prefix):
    root = os.environ.get('TMPDIR') or '/tmp'
    if not os.path.isabs(root):
        root = os.path.join(os.getcwd(), root)

    # tempfile adds eight ASCII characters. The daemon appends two 16-hex hashes and '.sock'.
    socket_name = '0' * 16 + '-' + '0' * 16 + '.sock'
    socket_path = os.path.join(root, prefix + 'xxxxxxxx', socket_name)
    # sockaddr_un.sun_path includes the terminating NUL (see src/daemon/posix.cppm).
    capacity = 108 if sys.platform == 'linux' else 104
    return root if len(os.fsencode(socket_path)) < capacity else '/tmp'
