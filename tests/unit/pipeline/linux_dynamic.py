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


DT_GNU_HASH = 0x6ffffef5

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

UNDEFINED = '''
extern void optional(void) __attribute__((weak));
__attribute__((used)) static void (*const reference)(void) = optional;
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


def provider_tables(image):
    phoff, = struct.unpack_from('<Q', image, 32)
    phentsize, phnum = struct.unpack_from('<HH', image, 54)
    headers = [struct.unpack_from('<IIQQQQQQ', image, phoff + i * phentsize) for i in range(phnum)]
    dynamic, = (p for p in headers if p[0] == 2)  # PT_DYNAMIC
    entries = {}
    for offset in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        tag, value = struct.unpack_from('<QQ', image, offset)
        if tag == 0:
            break
        entries[tag] = (value, offset)

    def table_offset(tag):
        address = entries[tag][0]
        file_offset, = (p[2] + address - p[3] for p in headers
                        if p[0] == 1 and p[3] <= address < p[3] + p[5])
        return file_offset

    return entries, table_offset


def edited(image, offset, fmt, *values):
    changed = bytearray(image)
    struct.pack_into(fmt, changed, offset, *values)
    return changed


def sectionless(image):
    return edited(edited(image, 40, '<Q', 0), 58, '<HHH', 0, 0, 0)


def main():
    ycxx, cc, readelf = sys.argv[1:]
    target = '--target=x86_64-pc-linux-gnu'
    soname = 'libycxx-leaf.so.1'
    extra = 'libycxx-extra.so.2'
    with tempfile.TemporaryDirectory(prefix='ycxx-linux-dynamic-', dir='.') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temp).resolve()
        (scope / '.ycxx').touch()
        for name in ('link', 'first', 'second', 'gnu', 'both', 'empty'):
            (scope / name).mkdir()
        base = {k: v for k, v in os.environ.items() if k not in
                ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT', 'LD_PRELOAD', 'LD_AUDIT')}
        base.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')
        cold = dict(base, YCXX_NO_DAEMON='1')
        daemon = dict(base, YCXX_DAEMON='require')

        def run(args, status=0, error='', env=cold):
            result = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=True, timeout=45)
            expected_status = result.returncode != 0 if status is None else result.returncode == status
            if not expected_status or error not in result.stderr:
                raise AssertionError(f'{args} exited {result.returncode}, expected {status if status is not None else "nonzero"}'
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
        missing = run(['./program', 'Q'], status=None, error=soname, env=dict(cold, LD_LIBRARY_PATH=''))
        assert missing.stdout == ''
        cxx(*options, inputs[0], f'first/{soname}', inputs[2], '-o', 'relocated')
        assert (scope / 'relocated').read_bytes() == image
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
        source('version.map', 'LEAF_1 { global: step; mix; local: *; };')
        provider('version.so', flags=('-Wl,--version-script,version.map',))
        reject(*options, 'main.cpp', 'version.so', error='versioned ELF shared object symbols')
        provider('depends.so', flags=('-Wl,--no-as-needed', 'link/extra.so'))
        reject(*options, 'main.cpp', 'depends.so', error='ELF shared object dependencies are unsupported')

        # Several real buckets and collisions exercise the contiguous GNU chain layout.
        source('gnu.c', PROVIDER + UNDEFINED + '\n'.join(f'int helper{i}(void) {{ return {i}; }}' for i in range(24)))
        gnu_providers = {}
        for style, bias in (('gnu', 0), ('both', 10)):
            path = scope / 'link' / f'{style}.so'
            provider(path, bias=bias, hash_style=style, src='gnu.c')
            obj = sectionless(path.read_bytes())
            entries, table_offset = provider_tables(obj)
            assert (4 in entries) == (style == 'both')
            gnu_providers[style] = obj
            if style == 'both':
                # A corrupted SysV bucket does not affect the loader's GNU lookup.
                obj = edited(obj, table_offset(4) + 8, '<I', 0xffffffff)
            path.write_bytes(obj)
            installed = scope / style
            (installed / soname).write_bytes(obj)
            shutil.copyfile(scope / 'link/extra.so', installed / extra)
            cxx(*options, 'main.cpp', path, 'link/extra.so', '-o', 'program')
            assert (scope / 'program').read_bytes() == image
            execute(style, 53 + bias)
            reject(*options, 'data.cpp', path, error="unsupported ELF import 'exported_data'")

        # The GNU symbol origin is not a full dynsym count when every bucket is empty.
        source('empty.c', UNDEFINED)
        provider('link/empty.so', hash_style='gnu', src='empty.c')
        undefined = re.search(r'^\s*(\d+):\s+.*\bUND\s+optional\b',
                              run([readelf, '--dyn-syms', '-W', 'link/empty.so']).stdout, re.MULTILINE)
        empty = sectionless((scope / 'link/empty.so').read_bytes())
        _entries, table_offset = provider_tables(empty)
        gnu_offset = table_offset(DT_GNU_HASH)
        bucket_count, first_symbol, bloom_count, _shift = struct.unpack_from('<4I', empty, gnu_offset)
        assert undefined and int(undefined.group(1)) > 0
        assert not any(struct.unpack_from(f'<{bucket_count}I', empty, gnu_offset + 16 + 8 * bloom_count))
        # Some producers put symoffset after the undefined prefix even when the table is empty.
        empty = edited(empty, gnu_offset + 4, '<I', min(first_symbol, int(undefined.group(1))))
        (scope / 'link/empty.so').write_bytes(empty)
        (scope / 'empty' / soname).write_bytes(empty)
        cxx(*options, 'no-imports.cpp', 'link/empty.so', '-o', 'empty-program')
        run(['./empty-program'], status=21, env=dict(cold, LD_LIBRARY_PATH=str(scope / 'empty')))
        reject(*options, 'main.cpp', 'link/empty.so', error="undefined symbol 'step'")

        # Modify only mapped metadata of valid sectionless providers.
        gnu = gnu_providers['gnu']
        both = gnu_providers['both']
        entries, table_offset = provider_tables(gnu)
        gnu_offset = table_offset(DT_GNU_HASH)
        bucket_count, first_symbol, bloom_count, _shift = struct.unpack_from('<4I', gnu, gnu_offset)
        bloom_offset = gnu_offset + 16
        bucket_offset = bloom_offset + 8 * bloom_count
        buckets = struct.unpack_from(f'<{bucket_count}I', gnu, bucket_offset)
        nonempty = [(i, start) for i, start in enumerate(buckets) if start]
        chain_offset = bucket_offset + 4 * bucket_count
        assert len(nonempty) > 1 and first_symbol > 1
        assert any(struct.unpack_from('<I', gnu, chain_offset + 4 * (start - first_symbol))[0] & 1 == 0
                   for _, start in nonempty)
        first_bucket = bucket_offset + 4 * nonempty[0][0]
        second_bucket = bucket_offset + 4 * nonempty[1][0]
        first_hash, = struct.unpack_from('<I', gnu, chain_offset)

        both_entries, both_table = provider_tables(both)
        both_gnu, both_sysv = both_table(DT_GNU_HASH), both_table(4)
        both_buckets, both_first, both_bloom, _shift = struct.unpack_from('<4I', both, both_gnu)
        both_symbols, = struct.unpack_from('<I', both, both_sysv + 4)
        both_bucket_offset = both_gnu + 16 + 8 * both_bloom
        last_hash = both_bucket_offset + 4 * (both_buckets + both_symbols - both_first - 1)
        end_hash, = struct.unpack_from('<I', both, last_hash)
        assert end_hash & 1
        # An empty GNU table also takes precedence over otherwise discoverable SysV names.
        dual_empty = edited(both, both_bucket_offset, f'<{both_buckets}I', *([0] * both_buckets))
        (scope / 'link/dual-empty.so').write_bytes(dual_empty)
        reject(*options, 'main.cpp', 'link/dual-empty.so', error="undefined symbol 'step'")
        # Reuse the SysV dynamic entry for an independent GNU-only DT_SYMTABSZ bound.
        sysv_entry = both_entries[4][1]
        (scope / 'link/sized.so').write_bytes(edited(both, sysv_entry, '<QQ', 39, 24 * both_symbols))
        cxx(*options, 'main.cpp', 'link/sized.so', 'link/extra.so', '-o', 'program')
        assert (scope / 'program').read_bytes() == image

        malformed = (
            ('address', edited(gnu, entries[DT_GNU_HASH][1] + 8, '<Q', 1 << 63), 'invalid mapped ELF shared object DT_GNU_HASH'),
            ('bloom-count', edited(gnu, gnu_offset + 8, '<I', 3), 'invalid ELF shared object DT_GNU_HASH dimensions'),
            ('bloom-bounds', edited(gnu, gnu_offset + 8, '<I', 1 << 31), 'invalid ELF shared object DT_GNU_HASH dimensions'),
            ('bloom-shift', edited(gnu, gnu_offset + 12, '<I', 32), 'invalid ELF shared object DT_GNU_HASH dimensions'),
            ('bloom-bits', edited(gnu, bloom_offset, f'<{bloom_count}Q', *([0] * bloom_count)), 'missing DT_GNU_HASH Bloom bits'),
            ('bucket-low', edited(gnu, first_bucket, '<I', first_symbol - 1), 'invalid ELF shared object DT_GNU_HASH symbol index'),
            ('bucket-high', edited(gnu, first_bucket, '<I', 0xffffffff), 'invalid ELF shared object DT_GNU_HASH symbol index'),
            ('bucket-gap', edited(gnu, first_bucket, '<I', 0), 'invalid ELF shared object DT_GNU_HASH chain ownership'),
            ('bucket-overlap', edited(gnu, second_bucket, '<I', first_symbol), 'invalid ELF shared object DT_GNU_HASH chain ownership'),
            ('chain-hash', edited(gnu, chain_offset, '<I', first_hash ^ 2), 'incorrect DT_GNU_HASH chain hash'),
            ('chain-end', edited(both, last_hash, '<I', end_hash & ~1), 'DT_GNU_HASH has no chain terminator'),
            ('short-sysv', edited(both, both_sysv + 4, '<I', both_first), 'invalid ELF shared object DT_GNU_HASH symbol index'),
            ('symtab-width', edited(both, sysv_entry, '<QQ', 39, 24 * both_symbols - 1), 'invalid ELF shared object DT_SYMTABSZ'),
            ('symtab-count', edited(both, sysv_entry, '<QQ', 39, 24 * (both_symbols + 1)),
             'DT_GNU_HASH disagrees with the dynamic symbol count'),
            ('name', edited(gnu, table_offset(6) + 24 * first_symbol, '<I', entries[10][0]), 'symbol name is outside the string table'),
        )
        for name, obj, error in malformed:
            (scope / f'bad-{name}.so').write_bytes(obj)
            reject(*options, 'main.cpp', f'bad-{name}.so', error=error)

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
            for style, status in (('gnu', 53), ('both', 63)):
                (scope / 'link/leaf.input').write_bytes(gnu_providers[style])
                replaced = daemon_link()
                assert any(e.get('kind') == 'Link' and e.get('outcome') == 'executed' for e in replaced), replaced
                execute(style, status)
            for name, _obj, error in malformed:
                reject(*options, 'main.cpp', f'bad-{name}.so', error=error, env=daemon)
        finally:
            run([ycxx, 'ctl', 'shutdown'], env=base)


if __name__ == '__main__':
    main()
