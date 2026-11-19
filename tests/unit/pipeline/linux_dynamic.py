"""Exercise ycxx's sectionless ET_EXEC through the native Linux x86-64 loader."""

import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile


PROVIDER = r'''
static int state;

static void mark(char byte) {
    long written;
    __asm__ volatile("syscall" : "=a"(written)
                     : "0"(1L), "D"(1L), "S"(&byte), "d"(1L)
                     : "rcx", "r11", "memory");
}

__attribute__((constructor)) static void initialize(void) {
    state = 7;
    mark('L');
}

__attribute__((destructor)) static void finalize(void) {
    mark(state == 9 ? 'F' : 'f');
}

int step(int expected, int next) {
    int previous = state;
    mark(previous == expected ? 'E' : 'e');
    state = next;
    return previous;
}

int mix(int a, ...) {
    __builtin_va_list rest;
    __builtin_va_start(rest, a);
    int value = state + a;
    for (int i = 2; i <= 8; ++i) value += i * __builtin_va_arg(rest, int);
    __builtin_va_end(rest);
    mark('M');
    return value + BIAS;
}

int exported_data = 10;
'''

MAIN = '''
extern "C" int step(int, int);
extern "C" int mix(int, ...);
static int initialized = step(7, 8);

int main(int argc, char** argv) {
    if (argc != 2 || argv[0] == nullptr || argv[1][0] != 'Q' || argv[argc] != nullptr) return 1;
    if (step(8, 9) != 8 || initialized != 7) return 2;
    return mix(1, 2, 3, 4, 5, 6, 7, 8) - 160;
}
'''


def inspect_image(image, loader):
    assert image[:7] == b'\x7fELF\2\1\1'
    assert struct.unpack_from('<HH', image, 16) == (2, 62)  # ET_EXEC, EM_X86_64
    entry, phoff, shoff = struct.unpack_from('<QQQ', image, 24)
    assert (phoff, shoff) == (64, 0)
    assert struct.unpack_from('<4H', image, 52) == (64, 56, 7, 0)
    assert struct.unpack_from('<2H', image, 60) == (0, 0)
    headers = [struct.unpack_from('<IIQQQQQQ', image, phoff + i * 56) for i in range(7)]
    assert [p[0] for p in headers] == [6, 3, 1, 1, 1, 2, 0x6474e551]
    assert [p[1] for p in headers] == [4, 4, 4, 5, 6, 6, 6]
    phdr, interp, read_only, code, data, dynamic, _stack = headers
    assert phdr[2] == phoff and phdr[5] == 7 * 56 and phdr[3] == read_only[3] + phoff
    assert image[interp[2]:interp[2] + interp[5]] == os.fsencode(loader) + b'\0'
    assert read_only[2] == 0 and read_only[5] >= interp[2] + interp[5]
    assert code[3] <= entry < code[3] + code[5]
    end = 0
    for _type, _flags, offset, address, _paddr, filesz, memsz, align in (read_only, code, data):
        assert align == 4096 and offset % align == address % align == 0
        assert offset >= end and address == read_only[3] + offset
        assert filesz <= memsz and offset + filesz <= len(image)
        end = offset + memsz
    assert data[3] <= dynamic[3] < dynamic[3] + dynamic[5] <= data[3] + data[5]
    entries = [struct.unpack_from('<QQ', image, p) for p in range(dynamic[2], dynamic[2] + dynamic[5], 16)]
    assert [tag for tag, _ in entries] == [1, 1, 5, 10, 6, 11, 4, 7, 8, 9, 0]
    tags = dict(entries)
    assert tags[11] == tags[9] == 24 and tags[8] == 2 * 24
    for tag in (5, 6, 4, 7):  # STRTAB, SYMTAB, HASH, RELA
        assert read_only[3] <= tags[tag] < read_only[3] + read_only[5]
    strings = tags[5] - read_only[3]
    symbols = tags[6] - read_only[3]
    assert image[strings] == 0 and image[symbols:symbols + 24] == bytes(24)
    slots = set()
    for index, name in enumerate((b'mix', b'step'), start=1):
        name_offset, info, other, section, value, size = struct.unpack_from('<IBBHQQ', image, symbols + index * 24)
        assert (info, other, section, value, size) == (0x12, 0, 0, 0, 0)
        assert image[strings + name_offset:strings + tags[10]].split(b'\0', 1)[0] == name
        slot, info, addend = struct.unpack_from('<QQq', image, tags[7] - read_only[3] + (index - 1) * 24)
        assert info == (index << 32) | 6 and addend == 0  # R_X86_64_GLOB_DAT
        assert slot % 8 == 0 and data[3] <= slot <= data[3] + data[5] - 8
        assert slot not in slots
        slots.add(slot)


def main():
    ycxx, cc, readelf = sys.argv[1:]
    target = '--target=x86_64-pc-linux-gnu'
    soname = 'libycxx-leaf.so.1'
    extra = 'libycxx-extra.so.2'
    with tempfile.TemporaryDirectory(prefix='ycxx-linux-dynamic-', dir='.') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temp).resolve()
        (scope / '.ycxx').touch()
        for name in ('link', 'first', 'second'):
            (scope / name).mkdir()
        base = {k: v for k, v in os.environ.items() if k not in
                ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT', 'LD_PRELOAD', 'LD_AUDIT')}
        base.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')
        cold = dict(base, YCXX_NO_DAEMON='1')
        daemon = dict(base, YCXX_DAEMON='require')

        def run(args, status=0, error='', env=cold):
            result = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=True, timeout=45)
            if result.returncode != status or error not in result.stderr:
                raise AssertionError(f'{args} exited {result.returncode}, expected {status}'
                                     f' and stderr containing {error!r}\n{result.stdout}{result.stderr}')
            return result

        def cxx(*args, **kwargs):
            return run([ycxx, 'cxx', *args], **kwargs)

        def source(name, text):
            (scope / name).write_text(text + '\n')

        def provider(out, name=soname, *, bias=0, hash_style='sysv', src='leaf.c', flags=()):
            soname_flag = [f'-Wl,-soname,{name}'] if name is not None else []
            run([cc, '-shared', '-fPIC', '-nostdlib', f'-Wl,--hash-style={hash_style}', *soname_flag,
                 f'-DBIAS={bias}', src, *flags, '-o', out])

        # Discover the native interpreter from a black-box host executable's program headers.
        source('probe.c', 'int main(void) { return 0; }')
        run([cc, 'probe.c', '-o', 'probe'])
        headers = run([readelf, '-lW', 'probe']).stdout
        match = re.search(r'\[Requesting program interpreter:\s*(/[^\]]+)\]', headers)
        assert match, headers
        loader = match.group(1)
        options = (target, '-nostdlib', f'--dynamic-linker={loader}')
        inputs = ('main.cpp', 'link/leaf.input', 'link/extra.so')
        source('main.cpp', MAIN)
        source('leaf.c', PROVIDER)
        source('extra.c', 'int extra(void) { return 0; }')
        provider('link/leaf.input')
        provider('link/extra.so', extra, src='extra.c')
        provider(f'second/{soname}', bias=10)
        for directory in ('first', 'second'):
            shutil.copyfile(scope / 'link/extra.so', scope / directory / extra)
        linked_provider = (scope / 'link/leaf.input').read_bytes()
        alternate = (scope / 'second' / soname).read_bytes()

        result = cxx(*options, *inputs, '-o', 'program', env=dict(cold, PATH=''))
        assert result.stdout == ''  # linking did not run the provider's initializer
        image = (scope / 'program').read_bytes()
        inspect_image(image, loader)
        details = run([readelf, '-W', '-h', '-l', '-d', '-D', '-r', 'program']).stdout
        assert re.findall(r'\bNEEDED\b[^\n]*\[([^\]]+)\]', details) == [soname, extra], details
        glob_dat = [line for line in details.splitlines() if 'R_X86_64_GLOB_DAT' in line]
        assert len(glob_dat) == 2 and 'mix' in glob_dat[0] and 'step' in glob_dat[1], details
        assert 'INIT_ARRAY' not in details and 'JMPREL' not in details

        def execute(directory, status, out='LEEMF'):
            result = run(['./program', 'Q'], status=status, env=dict(cold, LD_LIBRARY_PATH=str(scope / directory)))
            assert result.stdout == out and result.stderr == '', (result.stdout, result.stderr)

        # The installed SONAME can be located independently of the link-time path.
        (scope / 'link/leaf.input').rename(scope / 'first' / soname)
        execute('first', 53)
        execute('second', 63)
        (scope / 'link/leaf.input').write_bytes(linked_provider)
        source('no-imports.cpp', 'int main() { return 21; }')
        cxx(*options, 'no-imports.cpp', 'link/leaf.input', '-o', 'no-imports')
        result = run(['./no-imports'], status=21, env=dict(cold, LD_LIBRARY_PATH=str(scope / 'first')))
        assert result.stdout == 'Lf' and result.stderr == ''
        cxx(target, '-c', 'main.cpp')
        cxx(*options, 'main.o', *inputs[1:], '-o', 'objects')
        assert (scope / 'objects').read_bytes() == image

        # An explicit interpreter is a target path. It need not exist on the build host.
        missing_loader = '/ycxx/nonexistent/target/ld.so'
        cxx(target, '-nostdlib', '--dynamic-linker', missing_loader, *inputs, '-o', 'cross')
        assert missing_loader in run([readelf, '-lW', 'cross']).stdout

        def reject(*args, error, env=cold):
            cxx(*args, '-o', 'rejected', status=1, error=error, env=env)
            assert not (scope / 'rejected').exists()

        reject(target, '-nostdlib', 'main.cpp', 'link/extra.so', error="absolute '--dynamic-linker' path")
        reject(*options, 'main.cpp', error='explicit shared object')
        reject(target, '-nostdlib', '--dynamic-linker=relative', *inputs, error="absolute '--dynamic-linker' path")
        reject(target, '-nostdlib', 'main.cpp', '--dynamic-linker', error="argument to '--dynamic-linker' is missing")
        reject(target, f'--dynamic-linker={loader}', *inputs, error="requires '-nostdlib'")
        reject(f'--dynamic-linker={loader}', *inputs, error="'--dynamic-linker' requires an x86-64 Linux executable link")
        reject(*options, '-c', *inputs, error="'--dynamic-linker' requires an x86-64 Linux executable link")
        reject(*options, 'main.cpp', 'absent.so', error="cannot read 'absent.so'")
        reject(*options, 'main.cpp', 'link/extra.so', error="undefined symbol 'step'")
        reject(*options, *inputs, f'second/{soname}', error=f"duplicate ELF shared object SONAME '{soname}'")
        source('start.cpp', 'extern "C" void _start() {}')
        reject(*options, 'start.cpp', 'link/extra.so', error="a custom '_start' is unsupported for dynamic ELF linking")
        source('data.cpp', 'extern "C" int exported_data(); int main() { return exported_data(); }')
        reject(*options, 'data.cpp', 'link/leaf.input', error="unsupported ELF import 'exported_data'")
        source('script.so', 'GROUP ( link/leaf.input )')
        reject(*options, 'main.cpp', 'script.so', error='expected a little-endian x86-64 ELF shared object')
        provider('no-soname.so', None)
        reject(*options, 'main.cpp', 'no-soname.so', error='missing DT_SONAME')
        provider('slash.so', 'a/leaf.so')
        reject(*options, 'main.cpp', 'slash.so', error='nonempty and slash-free')
        provider('gnu.so', hash_style='gnu')
        reject(*options, 'main.cpp', 'gnu.so', error='GNU-hash-only')
        source('version.map', 'LEAF_1 { global: step; mix; local: *; };')
        provider('version.so', flags=('-Wl,--version-script,version.map',))
        reject(*options, 'main.cpp', 'version.so', error='versioned ELF shared object symbols')
        provider('depends.so', flags=('-Wl,--no-as-needed', 'link/extra.so'))
        reject(*options, 'main.cpp', 'depends.so', error='ELF shared object dependencies are unsupported')

        def daemon_link():
            report = cxx('--trace-engine=json', *options, *inputs, '-o', 'program', env=daemon)
            assert report.stdout == ''
            assert (scope / 'program').read_bytes() == image
            return [json.loads(line) for line in report.stderr.splitlines() if line.strip()]

        try:
            first = daemon_link()
            assert any(e.get('kind') == 'Link' and e.get('outcome') == 'executed' for e in first), first
            second = daemon_link()
            assert any(e.get('kind') == 'Build' and e.get('outcome') == 'reused' for e in second), second
            # Same metadata, same path, different code: the Link input still changes.
            (scope / 'link/leaf.input').write_bytes(alternate)
            replaced = daemon_link()
            assert any(e.get('kind') == 'Link' and e.get('outcome') == 'executed' for e in replaced), replaced
            execute('first', 53)
            execute('second', 63)
            reject(*options, 'main.cpp', 'gnu.so', error='GNU-hash-only', env=daemon)
        finally:
            run([ycxx, 'ctl', 'shutdown'], env=base)


if __name__ == '__main__':
    main()
