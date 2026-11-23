"""Itanium base, complete and deleting destructor entries for the pinned #999 shape."""

import argparse
from collections import namedtuple
from pathlib import Path
import re
import shutil
import subprocess
from tempfile import TemporaryDirectory

from process import Env, failures


TARGET = '--target=x86_64-pc-linux-gnu'
NATIVE_FLAGS = ('-std=c++26', '-pedantic-errors', '-O0', '-fsized-deallocation', '-fno-pie')
PIN_FLAGS = ('-std=c++26', '-pedantic-errors', '-nostdinc++', '-frtti', '-fPIC')
FIXTURES = Path(__file__).resolve().with_suffix('')
D0, D1, D2 = ('_ZN1DD0Ev', '_ZN1DD1Ev', '_ZN1DD2Ev')
B1, B2 = ('_ZN1BD1Ev', '_ZN1BD2Ev')
TABLE, RTTI, TYPE_NAME = ('_ZTV1D', '_ZTI1D', '_ZTS1D')
SINGLE_INHERITANCE = '_ZTVN10__cxxabiv120__si_class_type_infoE'
Symbol = namedtuple('Symbol', 'value size kind section')


def run(args, cwd):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (args, result.returncode, result.stdout, result.stderr)
    return result.stdout


def symbols(readelf, scope, obj):
    result = {}
    for line in run([readelf, '-Ws', obj], scope).splitlines():
        row = line.split()
        if len(row) >= 8 and row[0].endswith(':') and row[0][:-1].isdigit():
            result[row[7]] = Symbol(int(row[1], 16), int(row[2]), row[3], row[6])
    return result


def elf_header(readelf, scope, obj):
    text = run([readelf, '-Wh', obj], scope)
    assert all(value in text for value in ('ELF64', 'little endian', 'REL (Relocatable file)',
                                           'Advanced Micro Devices X86-64')), text


def relocations(readelf, scope, obj):
    sections = dict(re.findall(r'^\s*\[\s*(\d+)\]\s+(\S+)', run([readelf, '-WS', obj], scope), re.M))
    result = {}
    target = None
    for line in run([readelf, '-Wr', obj], scope).splitlines():
        header = re.match(r"Relocation section '\.rela(\.[^']+)'", line)
        if header:
            target = header[1]
        fields = line.split()
        if target and len(fields) >= 7 and fields[2] == 'R_X86_64_64':
            addend = int(fields[6], 16) * (1 if fields[5] == '+' else -1)
            result[target, int(fields[0], 16)] = fields[4], addend
    return sections, result


def check_owner(readelf, scope, obj):
    elf_header(readelf, scope, obj)
    sym = symbols(readelf, scope, obj)
    assert sym[D1] == sym[D2], sym
    for name in (D0, D1, D2):
        assert sym[name].kind == 'FUNC' and sym[name].size > 0 and sym[name].section.isdigit(), sym
    complete, deleting = sym[D1], sym[D0]
    if complete.section == deleting.section:
        assert (deleting.value + deleting.size <= complete.value or
                complete.value + complete.size <= deleting.value), sym
    assert sym[B2].section == 'UND' and B1 not in sym, sym
    assert all(sym[name].section == 'UND' for name in ('_ZTI1B', '_ZdlPvm', SINGLE_INHERITANCE)), sym
    assert sym[TABLE].size == 32 and sym[RTTI].size == 24, sym

    sections, rel = relocations(readelf, scope, obj)
    # Itanium §2.5.2: offset-to-top, RTTI, then the complete and deleting entries.
    # §2.9.5: the SI descriptor holds the runtime vptr, name and peer base descriptor.
    for owner, offset, symbol, addend in (
        (TABLE, 8, RTTI, 0), (TABLE, 16, D1, 0), (TABLE, 24, D0, 0),
        (RTTI, 0, SINGLE_INHERITANCE, 16), (RTTI, 8, TYPE_NAME, 0), (RTTI, 16, '_ZTI1B', 0),
    ):
        key = sections[sym[owner].section], sym[owner].value + offset
        assert rel.get(key) == (symbol, addend), (owner, offset, symbol, addend, rel)


def compile_native(compiler, scope, part, obj):
    run([compiler, *NATIVE_FLAGS, '-c', str(FIXTURES / f'{part}.cpp'), '-o', obj], scope)


def check_peers(readelf, scope, peer, consumer):
    base = symbols(readelf, scope, peer)
    use = symbols(readelf, scope, consumer)
    assert base[B2].section.isdigit() and base['_ZTI1B'].section.isdigit(), base
    assert use[D1].section == use[D2].section == 'UND', use


def link_both(compiler, scope, owner, peer, consumer):
    for objects in ((owner, peer, consumer), (consumer, peer, owner)):
        run([compiler, '-no-pie', *objects, '-o', 'probe-bin'], scope)
        run(['./probe-bin'], scope)


def native_oracles(compilers, readelf):
    with TemporaryDirectory(prefix='ycxx-native-derived-dtor-') as tmp:
        scope = Path(tmp)
        triples = []
        for index, compiler in enumerate(compilers):
            owner, peer, consumer = [f'{part}-{index}.o' for part in ('owner', 'peer', 'consumer')]
            for part, obj in zip(('owner', 'peer', 'consumer'), (owner, peer, consumer)):
                compile_native(compiler, scope, part, obj)
            check_owner(readelf, scope, owner)
            check_peers(readelf, scope, peer, consumer)
            run([compiler, *PIN_FLAGS, '-fsyntax-only', str(FIXTURES / 'pinned_probe.cpp')], scope)
            run([compiler, *PIN_FLAGS, '-c', str(FIXTURES / 'pinned_probe.cpp'), '-o', f'pin-{index}.o'], scope)
            elf_header(readelf, scope, f'pin-{index}.o')
            triples.append((compiler, owner, peer, consumer))
            print(f'{compiler}: ELF metadata and exact pinned CMake flags passed')
        for _, owner, _, _ in triples:
            for _, _, peer, _ in triples:
                for compiler, _, _, consumer in triples:
                    link_both(compiler, scope, owner, peer, consumer)
        print(f'Native owner/base/consumer matrix: {2 * len(triples) ** 3} runs exited zero')


def ycxx_tests(ycxx, host, readelf):
    world = Env(ycxx)
    scope = world.scope
    try:
        for name in ('layout.hpp', 'owner.cpp', 'pinned_probe.cpp'):
            shutil.copyfile(FIXTURES / name, scope / name)
        owner = scope / 'owner.cpp'
        original = owner.read_bytes()
        for env in (world.base, world.cold, world.daemon):
            env.update(LC_ALL='C', YCXX_THREADS='2')

        def built(state):
            args = [TARGET, '-std=c++26', '-c', 'owner.cpp', '-o', 'owner.o']
            images = []
            for env in (world.cold, world.daemon):
                obj = scope / 'owner.o'
                obj.unlink(missing_ok=True)
                result = world.cxx(args, scope, env, timeout=30)
                assert result == (0, b'', b''), (state, args, result)
                elf_header(readelf, scope, obj.name)
                images.append(obj.read_bytes())
            assert images[0] == images[1], state
            return images[0]

        baseline = built('key defined')
        pid = world.pid()
        check_owner(readelf, scope, 'owner.o')
        compile_native(host, scope, 'peer', 'peer.o')
        compile_native(host, scope, 'consumer', 'consumer.o')
        check_peers(readelf, scope, 'peer.o', 'consumer.o')
        link_both(host, scope, 'owner.o', 'peer.o', 'consumer.o')

        # [class.dtor] and Itanium §5.2.3: removing the key definition removes ownership.
        owner.write_bytes(b'#include "layout.hpp"\n')
        built('key declaration only')
        sym = symbols(readelf, scope, 'owner.o')
        assert all(name not in sym or sym[name].section == 'UND'
                   for name in (D0, D1, D2, TABLE, RTTI, TYPE_NAME)), sym
        world.same_daemon('declaration-only key', pid)
        owner.write_bytes(original)
        assert built('key definition restored') == baseline
        check_owner(readelf, scope, 'owner.o')
        world.same_daemon('restored key definition', pid)

        # Exact source from libycxx 4183795d CMakeLists.txt, lines 276–280.
        # This projection tests syntax and object output independently of driver mode flags.
        for mode in (('-fsyntax-only',), ('-c', '-o', 'pin.o')):
            result = world.cxx([TARGET, '-std=c++26', *mode, 'pinned_probe.cpp'], scope, world.cold, timeout=30)
            assert result[0] == 0, (mode, result)
        elf_header(readelf, scope, 'pin.o')
    finally:
        world.close()
    return bool(failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ycxx', nargs='?')
    parser.add_argument('host', nargs='?')
    parser.add_argument('readelf')
    parser.add_argument('--native', nargs=2, metavar=('GCC', 'CLANG'))
    args = parser.parse_args()
    if args.native:
        native_oracles(args.native, args.readelf)
        return 0
    if not args.ycxx or not args.host:
        parser.error('ycxx and host compiler are required without --native')
    return ycxx_tests(str(Path(args.ycxx).resolve()), args.host, args.readelf)


if __name__ == '__main__':
    raise SystemExit(main())
