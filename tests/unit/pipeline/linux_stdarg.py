"""Check the AMD64 psABI va_list type and glibc's partial stdarg.h request."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    ycxx, = sys.argv[1:]
    env = dict(os.environ, LC_ALL='C', YCXX_NO_DAEMON='1')
    env.pop('SDKROOT', None)
    with tempfile.TemporaryDirectory(prefix='ycxx-stdarg-') as temp:
        def compile(source, mode, target='x86_64-linux-gnu'):
            path = Path(temp) / 'probe.cpp'
            path.write_text(source)
            args = [ycxx, 'cxx', f'--target={target}', mode, str(path)]
            result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
            if result.returncode != 0:
                raise AssertionError(f'{args} exited {result.returncode}\n{result.stderr}\n{source}')
            return result.stdout

        compile('''\
struct __va_list_tag { unsigned gp_offset; };
typedef __builtin_va_list Builtin;
extern Builtin ap;
static_assert(sizeof(ap) == 24);
static_assert(alignof(Builtin) == 8);
static_assert(sizeof(ap) == sizeof(ap[0]));
static_assert(!__is_same(decltype(ap[0]), __va_list_tag&));
static_assert(__is_same(decltype(ap[0].gp_offset), unsigned int));
static_assert(__is_same(decltype(ap[0].fp_offset), unsigned int));
static_assert(__is_same(decltype(ap[0].overflow_arg_area), void*));
static_assert(__is_same(decltype(ap[0].reg_save_area), void*));
void adjusted(Builtin list);
void adjusted(decltype(&ap[0]) list) {}
static_assert(__is_same(decltype(&adjusted), void (*)(decltype(&ap[0]))));
namespace hides {
using __builtin_va_list = int;
static_assert(__is_same(__builtin_va_list, int));
static_assert(__is_same(::__builtin_va_list, Builtin));
}
''', '-fsyntax-only')

        for partial_first in (True, False):
            partial = '''\
#define __need___va_list
#include <stdarg.h>
#if defined(__need___va_list) || !defined(__GNUC_VA_LIST)
#error partial stdarg.h did not consume the request and expose __gnuc_va_list
#endif
static_assert(__is_same(__gnuc_va_list, __builtin_va_list));
'''
            public = '''\
#include <stdarg.h>
#include <stdarg.h>
#ifndef _VA_LIST_DEFINED
#error full stdarg.h did not expose va_list
#endif
static_assert(__is_same(va_list, __gnuc_va_list));
'''
            if partial_first:
                source = partial * 2 + '''\
#ifdef _VA_LIST_DEFINED
#error a partial include exposed the public typedef guard
#endif
using va_list = int;
'''
                compile(source, '-fsyntax-only')
                source = partial * 2 + public + partial
            else:
                source = public + partial * 2
            output = compile(source, '-E')
            for alias in ('__gnuc_va_list', 'va_list'):
                typedef = f'typedef __builtin_va_list {alias};'
                assert output.count(typedef) == 1, f'expected one {typedef}\n{output}'
            compile(source, '-fsyntax-only')

        if 'CS_GNU_LIBC_VERSION' in os.confstr_names and os.confstr('CS_GNU_LIBC_VERSION'):
            output = compile('#include <stdio.h>\n', '-E')
            assert 'typedef __builtin_va_list __gnuc_va_list;' in output, 'glibc did not receive __gnuc_va_list'
            assert 'vfprintf' in output, 'expected declarations from glibc stdio.h'

        compile('''\
static_assert(__is_same(__builtin_va_list, char*));
namespace hides {
using __builtin_va_list = int;
static_assert(__is_same(__builtin_va_list, int));
static_assert(__is_same(::__builtin_va_list, char*));
}
''', '-fsyntax-only', target='arm64-apple-darwin')


if __name__ == '__main__':
    main()
