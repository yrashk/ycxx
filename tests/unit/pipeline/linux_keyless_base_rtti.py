"""Check weak base RTTI and a peer-owned typed downcast on Linux x86-64."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from linux_compile import run
from linux_owned_derived_vtable import BASE, BASE_NAME, DERIVED, DERIVED_NAME, SI_RUNTIME, Elf, check_base
from linux_peer_derived_default_construction import native_compilers


HELPER = '__dynamic_cast'
TABLES = {'_ZTV1B', '_ZTV1D'}


def check_consumer(path):
    elf = Elf(path)
    check_base(elf)
    assert not set(elf.names) & (TABLES | {DERIVED_NAME}), (path, elf.names)
    for name in (DERIVED, HELPER):
        symbol = elf.symbol(name)
        assert (symbol.info >> 4, symbol.other, symbol.section) == (1, 0, 0), (path, name, symbol)
    references = [row.symbol for rows in elf.relocations.values() for row in rows]
    for name in (BASE, DERIVED, HELPER):
        assert references.count(name) == 1, (path, name, references)


def check_coalesced(path):
    elf = Elf(path)
    check_base(elf)
    # Itanium ABI §2.9.5. The keyed owner has a single public nonvirtual base at zero.
    descriptor = elf.object('_ZTI1E', 24, (0x11, 0x21))
    elf.check_words(descriptor, {0: (1, SI_RUNTIME, 16), 8: (1, '_ZTS1E', 0), 16: (1, BASE, 0)})
    table = elf.object('_ZTV1E', 24, (0x11, 0x21))
    elf.check_words(table, {8: (1, '_ZTI1E', 0), 16: (1, '_ZN1E1fEv', 0)})

    cast = elf.symbol('coalesced_downcast')
    assert cast.info == 0x12 and cast.section != 0 and cast.size > 0, (path, cast)
    references = [row.symbol for row in elf.relocations.get(cast.section, ())
                  if cast.value <= row.offset < cast.value + cast.size]
    # check_base requires one symbol-table entry for BASE, shared by the descriptor and this code.
    for name in (BASE, DERIVED, HELPER):
        assert references.count(name) == 1, (path, name, references)


def replay(ycxx, scope, env, cold):
    flags = [ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26']
    resident = dict(env, YCXX_DAEMON='require')
    header, source, obj = (scope / name for name in ('base.hpp', 'direct.cpp', 'direct.o'))
    definition, original = header.read_bytes(), source.read_bytes()

    def compile_in(context, supported):
        obj.unlink(missing_ok=True)
        run([*flags, '-fsyntax-only', source.name], scope, context)
        object_command = [*flags, '-c', source.name, '-o', obj.name]
        if supported:
            run(object_command, scope, context)
            return obj.read_bytes()
        result = subprocess.run(object_command, cwd=scope, env=context, capture_output=True, text=True, timeout=30)
        assert result.returncode == 1 and ': error:' in result.stderr and not obj.exists(), result
        return result.stdout, result.stderr

    def compare(*, supported=True):
        first = compile_in(cold, supported)
        assert first == compile_in(resident, supported)
        return first

    def pid():
        status = run([ycxx, 'ctl', 'status'], scope, env)
        found, = (line for line in status.splitlines() if line.startswith('pid: '))
        return found

    try:
        baseline = compare()
        initial_pid = pid()
        check_consumer(obj)

        # The late, same-unit key makes the destination owned. The cast remains well-formed.
        source.write_bytes(original + b'\nint D::f() { return payload; }\n')
        compare(supported=False)
        source.write_bytes(original)

        # A hidden base cannot use the default-visible coalesced descriptor.
        base = b'struct B {'
        assert definition.count(base) == 1
        header.write_bytes(definition.replace(base, b'struct __attribute__((visibility("hidden"))) B {'))
        compare(supported=False)
        header.write_bytes(definition)

        # With no cast in this translation unit its strong imports and weak groups disappear.
        cast = b'return dynamic_cast<D*>(source);'
        assert original.count(cast) == 1
        source.write_bytes(original.replace(cast, b'return nullptr;'))
        compare()
        without_cast = Elf(obj)
        assert not set(without_cast.names) & (TABLES | {BASE, BASE_NAME, DERIVED, DERIVED_NAME, HELPER}), (
            obj, without_cast.names)
        assert not any(section.kind == 17 for section in without_cast.sections), obj

        source.write_bytes(original)
        assert compare() == baseline and pid() == initial_pid
    finally:
        subprocess.run([ycxx, 'ctl', 'shutdown'], cwd=scope, env=env, capture_output=True, timeout=30)


def main():
    ycxx, host = sys.argv[1:]
    fixtures = Path(__file__).resolve().with_suffix('')
    with tempfile.TemporaryDirectory(prefix='ycxx-keyless-rtti-', dir='.') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-', dir='/tmp') as runtime:
        scope = Path(temp).resolve()
        (scope / '.ycxx').touch()
        env = {key: value for key, value in os.environ.items()
               if key not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT', 'YCXX_NO_WATCH')}
        env.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2', XDG_CACHE_HOME=str(scope / 'cache'))
        cold = dict(env, YCXX_NO_DAEMON='1')
        for name in ('base.hpp', 'direct.cpp', 'qualified.cpp'):
            shutil.copyfile(fixtures / name, scope / name)
        replay(ycxx, scope, env, cold)
        run([ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26', '-c', 'qualified.cpp',
             '-o', 'qualified.o'], scope, cold)
        check_consumer(scope / 'qualified.o')

        run([ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26', '-c',
             fixtures / 'coalesced.cpp', '-o', 'coalesced.o'], scope, cold)
        check_coalesced(scope / 'coalesced.o')

        for compiler in native_compilers(host, scope, cold):
            run([compiler, '-std=c++20', '-pedantic-errors', '-O0', '-fPIE', '-fno-lto',
                 '-c', fixtures / 'peer.cpp', '-o', 'peer.o'], scope, cold)
            for order in (('direct.o', 'qualified.o', 'peer.o'), ('peer.o', 'qualified.o', 'direct.o')):
                run([compiler, '-pie', '-fno-lto', '-Wl,-z,text,--wrap=__dynamic_cast',
                     *order, '-o', 'mixed'], scope, cold)
                run(['./mixed'], scope, cold)


if __name__ == '__main__':
    main()
