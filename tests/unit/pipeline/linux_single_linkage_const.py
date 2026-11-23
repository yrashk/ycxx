"""Check namespace const definitions and name linkage under [dcl.link]/8 and [basic.link]/3.2."""

import os
from pathlib import Path
import sys
import tempfile

from linux_compile import run


SUBJECT = '''\
extern "C" const char defined = 11;
extern "C++" const char also = 12;
extern "C" const char provided;

extern "C" {
  const char braced = 13;
  extern const char exported = 15;
}
const char plain = 14;

extern "C" const char* address_braced() { return &braced; }
extern "C" const char* address_plain() { return &plain; }
extern "C" const char* address_provided() { return &provided; }
'''

PEER = '''\
extern "C" const char defined;
extern "C++" const char also;
extern "C" const char exported;
extern "C" const char provided = 16;
extern "C" const char* address_braced();
extern "C" const char* address_plain();
extern "C" const char* address_provided();

int main() {
  if (defined != 11 || also != 12 || exported != 15) return 1;
  if (*address_braced() != 13 || *address_plain() != 14) return 2;
  const char* actual = address_provided();
  return actual == &provided && *actual == 16 ? 0 : 3;
}
'''


def check_symbols(readelf, obj, scope, env):
    table = run([readelf, '-Ws', obj], scope, env)
    symbols = [fields for line in table.splitlines()
               if len(fields := line.split()) == 8 and fields[0].endswith(':')]

    def entry(name, local=False):
        # A local can keep its source spelling or carry the Itanium source-name suffix.
        found = [row for row in symbols if row[7] == name or
                 (local and row[7].endswith(f'{len(name)}{name}'))]
        assert len(found) == 1, (name, found, table)
        return found[0]

    for name in ('defined', 'also', 'exported'):
        row = entry(name)
        assert row[4] == 'GLOBAL' and row[6].isdigit(), row
    for name in ('braced', 'plain'):
        row = entry(name, local=True)
        assert row[4] == 'LOCAL' and row[6].isdigit(), row
    imported = entry('provided')
    assert imported[4] == 'GLOBAL' and imported[6] == 'UND', imported
    relocations = run([readelf, '-Wr', obj], scope, env)
    assert any('provided' in line.split() for line in relocations.splitlines()), relocations


def main():
    ycxx, host, readelf = sys.argv[1:]
    env = dict(os.environ, LC_ALL='C')
    with tempfile.TemporaryDirectory(prefix='ycxx-single-linkage-', dir='.') as temp:
        scope = Path(temp)
        (scope / 'subject.cpp').write_text(SUBJECT)
        (scope / 'peer.cpp').write_text(PEER)
        compile_ycxx = [ycxx, 'cxx', '--no-daemon', '--target=x86_64-pc-linux-gnu', '-std=c++26']
        run([*compile_ycxx, '-fsyntax-only', 'subject.cpp'], scope, env)
        run([*compile_ycxx, '-c', 'subject.cpp', '-o', 'subject.o'], scope, env)
        check_symbols(readelf, 'subject.o', scope, env)
        run([host, '-std=c++26', '-O0', '-c', 'peer.cpp', '-o', 'peer.o'], scope, env)
        run([host, '-no-pie', 'subject.o', 'peer.o', '-o', 'mixed'], scope, env)
        run(['./mixed'], scope, env)


if __name__ == '__main__':
    main()
