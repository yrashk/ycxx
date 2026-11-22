"""Exchange virtual tables, RTTI and virtual calls with Clang peers on arm64 Darwin (#650).

The Mach-O counterpart of the linux_* virtual tests, over their fixtures: ycxx's objects are
linked with Clang-built peers by ycxx's linker and by the system linker (also dead-stripping),
and each program checks the objects' layout, slots and RTTI at run time. The Itanium C++ ABI
(§2.5, §2.9) is shared; what is Darwin's own is the `_` symbol prefix and Apple's arm64
convention for vague-linkage RTTI: a private-external descriptor whose name pointer has bit 63
set (non-unique), so that `type_info` compares such names by content across images.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def main():
    ycxx, host_cxx, nm = sys.argv[1:]
    fixtures = Path(__file__).resolve().parent
    env = {k: v for k, v in os.environ.items() if k not in ('YCXX_DAEMON', 'YCXX_ROOT')}
    env.update(LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='2')
    with tempfile.TemporaryDirectory(prefix='ycxx-darwin-virtual-') as temp:
        scope = Path(temp)
        (scope / '.ycxx').touch()

        def run(*args, cwd=scope):
            result = subprocess.run([str(a) for a in args], cwd=cwd, env=env, capture_output=True, text=True,
                                    timeout=120)
            if result.returncode != 0:
                raise AssertionError(f'{args} exited {result.returncode}\n{result.stdout}{result.stderr}')
            return result.stdout

        def fixture(name):
            """A copy of a linux_* fixture directory, so that edits stay in the scratch tree."""
            shutil.copytree(fixtures / name, scope / name)
            return scope / name

        def compile(directory, source, obj):
            run(ycxx, 'cxx', '-c', source, '-o', obj, cwd=directory)

        def peer(directory, source, obj, *flags):
            run(host_cxx, '-std=c++20', '-pedantic-errors', '-O0', *flags, '-c', source, '-o', obj, cwd=directory)

        def links(directory, *objects, expect=None):
            """Links with the system linker (also dead-stripping) and ycxx's, and runs each."""
            for name, linker in (('host', [host_cxx]), ('stripped', [host_cxx, '-Wl,-dead_strip']),
                                 ('ycxx', [ycxx, 'cxx'])):
                run(*linker, *objects, '-o', f'program-{name}', cwd=directory)
                output = run(f'./program-{name}', cwd=directory)
                if expect is not None:
                    assert expect(output), (directory.name, objects, name, output)

        def symbols(directory, obj):
            """{name: (section or None, binding, address)} from `nm -m`."""
            table = {}
            for line in run(nm, '-m', obj, cwd=directory).splitlines():
                m = re.match(r'^([0-9a-f]+)?\s*\(([^)]*)\)\s+((?:weak )?(?:private )?(?:non-)?external)'
                             r'(?: \[[^]]*\])?\s+(\S+)', line)
                if m:
                    table[m[4]] = (None if m[2] == 'undefined' else m[2], m[3], int(m[1] or '0', 16))
            return table

        def words(directory, obj, segment, section):
            """The section's bytes, by address, from `otool -s`."""
            data = {}
            for line in run('otool', '-s', segment, section, obj, cwd=directory).splitlines():
                m = re.match(r'^([0-9a-f]{16})\s+((?:[0-9a-f]{8}\s*)+)$', line)
                if not m:
                    continue
                address = int(m[1], 16)
                for word in m[2].split():
                    for k, byte in enumerate(int(word, 16).to_bytes(4, 'little')):
                        data[address + k] = byte
                    address += 4
            return data

        def pointer_word(data, address):
            return int.from_bytes(bytes(data[address + k] for k in range(8)), 'little')

        # A root that this unit owns through its key function, and a call through its table.
        root = fixture('linux_virtual_consumer') / 'owned-root'
        for part in ('first', 'virtual-call'):
            compile(root, f'{part}.cpp', f'{part}.o')
        table = symbols(root, 'first.o')
        assert table['__ZTVN6abi3094RootE'][:2] == ('__DATA,__const', 'external'), table
        assert table['__ZTIN6abi3094RootE'][:2] == ('__DATA,__const', 'external'), table
        assert table['__ZTSN6abi3094RootE'][:2] == ('__TEXT,__const', 'weak external'), table
        assert table['__ZTVN10__cxxabiv117__class_type_infoE'][0] is None, table
        assert not any(name.startswith(('__ZTV', '__ZTI', '__ZTS')) and entry[0]
                       for name, entry in symbols(root, 'virtual-call.o').items())
        peer(root, 'host.cpp', 'host.o', '-std=c++26', '-frtti', '-DEXPECT_FIRST=71')
        peer(root, 'second.cpp', 'second.o')
        links(root, 'host.o', 'first.o', 'second.o', 'virtual-call.o')

        # Calls on peer-owned roots and primary bases, a peer's layout, and a non-key member.
        consumer = scope / 'linux_virtual_consumer'
        for part in ('host', 'provider'):
            peer(consumer, f'{part}.cpp', f'{part}.o', '-std=c++26', '-frtti')
        compile(consumer, 'consumer.cpp', 'consumer.o')
        links(consumer, 'host.o', 'provider.o', 'consumer.o')
        source = (consumer / 'consumer.cpp').read_text()
        (consumer / 'first-slot.cpp').write_text(source.replace('object->alpha(argument)', 'object->zeta(argument)', 1))
        host = (consumer / 'host.cpp').read_text()
        (consumer / 'first-slot-host.cpp').write_text(host.replace('!= 207', '!= 107').replace('!= 413', '!= 313'))
        compile(consumer, 'first-slot.cpp', 'first-slot.o')
        peer(consumer, 'first-slot-host.cpp', 'first-slot-host.o', '-std=c++26', '-frtti')
        links(consumer, 'first-slot-host.o', 'provider.o', 'first-slot.o')
        for part in ('layout-consumer', 'primary', 'nonkey/measure'):
            compile(consumer, f'{part}.cpp', f'{Path(part).name}.o')
        measure = symbols(consumer, 'measure.o')
        assert measure['__ZNK5Gauge7measureEi'][:2] == ('__TEXT,__text', 'external'), measure
        assert not any(name.startswith(('__ZTV', '__ZTI', '__ZTS')) and entry[0] for name, entry in measure.items())
        for level in ('-O0', '-O2'):
            peer(consumer, 'layout-provider-host.cpp', 'layout-provider.o', level)
            links(consumer, 'layout-provider.o', 'layout-consumer.o', expect=lambda out: len(out.splitlines()) == 19)
            peer(consumer, 'primary.peer.cpp', 'primary-peer.o', level)
            links(consumer, 'primary-peer.o', 'primary.o', expect=lambda out: out == 'primary-base consumer ok\n')
            for part in ('peer', 'caller'):
                peer(consumer, f'nonkey/{part}.cpp', f'nonkey-{part}.o', level, '-frtti')
            links(consumer, 'nonkey-peer.o', 'nonkey-caller.o', 'measure.o')

        # A derived table with single-inheritance RTTI, and the base's vague-linkage descriptor.
        derived = fixture('linux_owned_derived_vtable')
        for part in ('owner', 'key', 'maker'):
            compile(derived, f'{part}.cpp', f'{part}.o')
        for obj in ('owner.o', 'key.o'):
            table = symbols(derived, obj)
            assert table['__ZTV1D'][:2] == ('__DATA,__const', 'external'), (obj, table)
            assert table['__ZTI1D'][:2] == ('__DATA,__const', 'external'), (obj, table)
            assert table['__ZTS1D'][:2] == ('__TEXT,__const', 'weak external'), (obj, table)
            assert table['__ZTI1B'][:2] == ('__DATA,__const', 'weak private external'), (obj, table)
            assert table['__ZTS1B'][:2] == ('__TEXT,__const', 'weak private external'), (obj, table)
            assert '__ZTV1B' not in table, (obj, table)
            # The base descriptor's name pointer: __ZTS1B with Apple's non-unique bit.
            data = words(derived, obj, '__DATA', '__const')
            assert pointer_word(data, table['__ZTI1B'][2] + 8) == 1 << 63, obj
            assert pointer_word(data, table['__ZTI1D'][2] + 8) == 0, obj
        maker = symbols(derived, 'maker.o')
        assert maker['__ZTV1D'][0] is None and '__ZTI1B' not in maker, maker
        flags = ('-fno-exceptions', '-fdata-sections', '-ffunction-sections')
        for part in ('dispatch', 'peer-opaque', 'peer-construct', 'peer-rtti'):
            peer(derived, f'{part}.cpp', f'{part}.o', *flags)
        peer(derived, 'key.cpp', 'native-key.o', *flags)
        native = symbols(derived, 'peer-rtti.o')
        assert native['__ZTI1B'][1] == 'weak private external', native  # the convention this follows
        links(derived, 'owner.o', 'dispatch.o', 'peer-opaque.o')
        links(derived, 'maker.o', 'key.o', 'dispatch.o', 'peer-opaque.o')
        links(derived, 'native-key.o', 'peer-rtti.o', 'peer-construct.o', 'dispatch.o')
        for first, second in (('key.o', 'peer-rtti.o'), ('peer-rtti.o', 'key.o')):
            links(derived, first, second, 'peer-construct.o', 'dispatch.o')
        # ycxx's base descriptor, reached through its own `__ZTI1D` (`lib.cpp` defines no copy of
        # `__ZTI1B`): in a linked program its name pointer keeps bit 63, and in a library it
        # compares equal to the host's own private copy at another address.
        (derived / 'lib.cpp').write_text(
            '#include "base.hpp"\n#include <typeinfo>\n'
            'const std::type_info& lib_derived() noexcept { return typeid(D); }\n')
        (derived / 'bit.cpp').write_text(
            '#include <cstdint>\n#include <cstdio>\n#include <cstring>\n#include <typeinfo>\n'
            'const std::type_info& lib_derived() noexcept;\n'
            'int main() {\n'
            '  const auto* base = reinterpret_cast<const std::type_info* const*>(&lib_derived())[2];\n'
            '  const auto name = reinterpret_cast<const std::uintptr_t*>(base)[1];\n'
            '  std::printf("%d %s\\n", int(name >> 63), base->name());\n'
            '}\n')
        (derived / 'image.cpp').write_text(
            '#include "base.hpp"\n#include <cstdio>\n#include <typeinfo>\n'
            'const std::type_info& lib_derived() noexcept;\n'
            'int main() {\n'
            '  const auto* base = reinterpret_cast<const std::type_info* const*>(&lib_derived())[2];\n'
            '  std::printf("%d %d\\n", int(*base == typeid(B)), int(base != &typeid(B)));\n'
            '}\n')
        for part in ('lib', 'bit', 'image'):
            peer(derived, f'{part}.cpp', f'{part}.o', '-frtti')
        assert '__ZTI1B' not in symbols(derived, 'lib.o')
        links(derived, 'bit.o', 'lib.o', 'key.o', expect=lambda out: out == '1 1B\n')
        run(host_cxx, '-dynamiclib', 'key.o', 'lib.o', '-o', 'libderived.dylib', cwd=derived)
        run(host_cxx, 'image.o', 'libderived.dylib', '-o', 'image', cwd=derived)
        assert run('./image', cwd=derived) == '1 1\n'

        # The deleting, complete and base-object destructors of a root with a peer key function.
        destructor = fixture('linux_root_virtual_destructor')
        compile(destructor, 'producer.cpp', 'producer.o')
        table = symbols(destructor, 'producer.o')
        for name in ('__ZN1RD0Ev', '__ZN1RD1Ev', '__ZN1RD2Ev'):
            assert table[name][:2] == ('__TEXT,__text', 'external'), (name, table)
        assert table['__ZN1RD2Ev'][2] == table['__ZN1RD1Ev'][2] != table['__ZN1RD0Ev'][2], table
        assert table['__ZTV1R'][0] is None and table['__ZdlPvm'][0] is None, table
        for part in ('peer', 'dispatch'):
            peer(destructor, f'{part}.cpp', f'{part}.o', '-Wall', '-Wextra', '-Werror', '-fno-exceptions',
                 '-fsized-deallocation')
        links(destructor, 'producer.o', 'peer.o', 'dispatch.o')
        links(destructor, 'dispatch.o', 'peer.o', 'producer.o')

        # Base-object constructor and destructor entries beside the complete-object ones.
        variants = fixture('linux_base_object_variants')
        compile(variants, 'provider.cpp', 'provider.o')
        table = symbols(variants, 'provider.o')
        for complete, base in (('__ZN1p4BaseC1Ei', '__ZN1p4BaseC2Ei'), ('__ZN1p4BaseD1Ev', '__ZN1p4BaseD2Ev')):
            assert table[complete][:2] == table[base][:2] == ('__TEXT,__text', 'external'), table
            assert table[complete][2] == table[base][2], (complete, base, table)
        # Controls: a layout the whole unit leaves uncertain (an aligned or mode-attributed alias
        # type) or a callback signature keeps the complete-object entry only.
        (variants / 'aligned_alias.cpp').write_text(
            'typedef int Wide __attribute__((aligned(32)));\n'
            'struct B { char head; Wide value; B(int); ~B(); };\n'
            'B::B(int n) : head(0), value(n) {}\nB::~B() {}\n')
        for source, complete, base in (
            ('aligned_alias.cpp', '__ZN1BC1Ei', '__ZN1BC2Ei'),
            ('aligned_alias.cpp', '__ZN1BD1Ev', '__ZN1BD2Ev'),
            ('wide_enum.cpp', '__ZN4BaseC1E5Value', '__ZN4BaseC2E5Value'),
            ('callback.cpp', '__ZN8CallbackC1EPFivE', '__ZN8CallbackC2EPFivE'),
        ):
            compile(variants, source, 'control.o')
            table = symbols(variants, 'control.o')
            assert table.get(complete, (None,))[0] == '__TEXT,__text' and base not in table, (source, table)
        for part in ('derived', 'direct', 'trace'):
            peer(variants, f'{part}.cpp', f'{part}.o', '-fno-exceptions', '-fno-rtti')
        links(variants, 'provider.o', 'derived.o', 'direct.o', 'trace.o')

        # Automatic objects whose vptr ycxx stores: a peer root, then a derived class.
        roots = fixture('linux_peer_root_default_construction')
        compile(roots, 'maker.cpp', 'maker.o')
        compile(roots, 'key.cpp', 'ycxx-key.o')
        for part in ('key', 'other', 'peer'):
            peer(roots, f'{part}.cpp', f'{part}.o', '-frtti', '-DEXPECT_COUNT=7')
        for key in ('key.o', 'ycxx-key.o'):
            objects = ['maker.o', key, 'other.o', 'peer.o']
            links(roots, *objects)
            links(roots, *reversed(objects))
        derived_maker = fixture('linux_peer_derived_default_construction')
        compile(derived_maker, 'maker.cpp', 'maker.o')
        for part in ('peer', 'dispatch'):
            peer(derived_maker, f'{part}.cpp', f'{part}.o')
        links(derived_maker, 'maker.o', 'peer.o', 'dispatch.o')

        # Dispatch through a pure interface, and through a `final` member.
        for name in ('linux_pure_virtual_dispatch', 'linux_virtual_member_final'):
            directory = fixture(name)
            compile(directory, 'consumer.cpp', 'consumer.o')
            peer(directory, 'peer.cpp', 'peer.o')
            links(directory, 'consumer.o', 'peer.o')


if __name__ == '__main__':
    main()
