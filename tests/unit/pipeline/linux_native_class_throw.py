"""Check native root-class exception RTTI and hosted catch on Linux x86-64."""

import argparse
import os
from pathlib import Path
import platform
import tempfile

from linux_compile import run
from linux_owned_derived_vtable import BASE_RUNTIME, Elf


FIXTURES = Path(__file__).resolve().with_suffix('')
DESCRIPTOR = '_ZTI5Fault'
TYPE_NAME = '_ZTS5Fault'
HOST_FLAGS = [
    '-std=c++26', '-pedantic-errors', '-Wall', '-Wextra', '-Werror',
    '-nostdinc', '-nostdinc++', '-O0', '-fexceptions', '-fPIC', '-fno-lto',
]


def check_producer(path):
    elf = Elf(path)
    # Itanium C++ ABI §2.9.5 and §5.2.4. EH needs the class descriptor in both RTTI modes.
    descriptor = elf.object(DESCRIPTOR, 16, (0x21,))  # STB_WEAK | STT_OBJECT
    name = elf.object(TYPE_NAME, 7, (0x21,))
    relocation_section, = elf.check_words(descriptor, {
        0: (1, BASE_RUNTIME, 16),  # R_X86_64_64, after two eight-byte vtable entries.
        8: (1, TYPE_NAME, 0),
    })
    elf.group(DESCRIPTOR, (descriptor.section, relocation_section))
    elf.group(TYPE_NAME, (name.section,))
    elf.check_words(name, {})

    imports = {'__cxa_allocate_exception', '__cxa_throw', BASE_RUNTIME, '_ZTIi'}
    for spelling in imports:
        symbol = elf.symbol(spelling)
        assert (symbol.info >> 4, symbol.other, symbol.section) == (1, 0, 0), (path, spelling, symbol)
    references = {row.symbol for rows in elf.relocations.values() for row in rows}
    assert imports | {DESCRIPTOR} <= references, (path, references)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gcc', default=os.environ.get('YCXX_GXX', 'g++'))
    parser.add_argument('--clang', default=os.environ.get('YCXX_CLANGXX', 'clang++'))
    parser.add_argument('--native-rtti', type=Path, help='ycxx producer object compiled with -frtti')
    parser.add_argument('--native-no-rtti', type=Path, help='ycxx producer object compiled with -fno-rtti')
    args = parser.parse_args()
    if bool(args.native_rtti) != bool(args.native_no_rtti):
        parser.error('provide both native producer objects or neither')
    # PARITY: #648 tracks native Darwin EH. These metadata checks target x86-64 ELF.
    if (platform.system(), platform.machine()) != ('Linux', 'x86_64'):
        parser.error('requires a Linux x86-64 host')

    env = dict(os.environ, LC_ALL='C')
    drivers = {'gcc': args.gcc, 'clang': args.clang}
    with tempfile.TemporaryDirectory(prefix='ycxx-class-throw-') as temp:
        scope = Path(temp)
        for label, driver in drivers.items():
            print(f'{label}: {run([driver, "--version"], scope, env).splitlines()[0]}')
            run([driver, *HOST_FLAGS, '-frtti', '-c', FIXTURES / 'peer.cpp',
                 '-o', f'{label}-peer.o'], scope, env)

        if args.native_rtti:
            producers = [('ycxx', 'frtti', args.native_rtti.resolve()),
                         ('ycxx', 'fno-rtti', args.native_no_rtti.resolve())]
        else:
            producers = []
            for label, driver in drivers.items():
                for mode in ('frtti', 'fno-rtti'):
                    obj = scope / f'{label}-{mode}.o'
                    run([driver, *HOST_FLAGS, f'-{mode}', '-c', FIXTURES / 'producer.cpp',
                         '-o', obj], scope, env)
                    producers.append((label, mode, obj))

        for producer, mode, obj in producers:
            check_producer(obj)
            print(f'{producer} -{mode}: class RTTI symbols, COMDAT groups and relocations passed')
            for peer, driver in drivers.items():
                peer_object = scope / f'{peer}-peer.o'
                for inputs in ((obj, peer_object), (peer_object, obj)):
                    order = 'producer first' if inputs[0] == obj else 'peer first'
                    run([driver, '-pie', '-fno-lto', *inputs, '-o', 'hosted'], scope, env)
                    run(['./hosted'], scope, env)
                    print(f'{producer} -{mode} with {peer} peer ({order}): passed')


if __name__ == '__main__':
    main()
