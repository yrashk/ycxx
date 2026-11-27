"""Link, load, and consume an arm64 Mach-O leaf dylib (#393)."""

import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from typing import NamedTuple

from darwin_cleanup_free_unwind import records


PROVIDER = '''
static unsigned private_counts[4097];

static int add(int value) { return value + BIAS; }
extern "C" __attribute__((visibility("hidden"))) int hidden_score(int value) { return add(value); }
extern "C" int score(int value) { return hidden_score(value); }
extern "C" const char* greeting() { return "native Mach-O leaf"; }
extern "C" unsigned bump(unsigned slot, unsigned delta) {
    unsigned previous = private_counts[slot];
    private_counts[slot] = previous + delta;
    return previous;
}
'''

PEER = '''
extern "C" {
    unsigned long long peer_count __attribute__((visibility("hidden")));
}
extern "C" unsigned long long bump_peer(unsigned long long delta) {
    unsigned long long previous = peer_count;
    peer_count = previous + delta;
    return previous;
}
'''

LOADER = r'''
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc != 3) return 1;
    void* image = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!image) {
        fprintf(stderr, "%s\n", dlerror());
        return 2;
    }
    int (*score)(int) = (int (*)(int))dlsym(image, "score");
    const char* (*greeting)(void) = (const char* (*)(void))dlsym(image, "greeting");
    unsigned (*bump)(unsigned, unsigned) = (unsigned (*)(unsigned, unsigned))dlsym(image, "bump");
    unsigned long long (*bump_peer)(unsigned long long) =
        (unsigned long long (*)(unsigned long long))dlsym(image, "bump_peer");
    if (!score || !greeting || !bump || !bump_peer ||
        dlsym(image, "hidden_score") || dlsym(image, "peer_count")) return 3;
    if (score(35) != atoi(argv[2]) || strcmp(greeting(), "native Mach-O leaf")) return 4;
    const unsigned long long large = 1ULL << 40;
    if (bump(0, 7) != 0 || bump(4096, 9) != 0 || bump_peer(large) != 0) return 5;
    if (bump(0, 1) != 7 || bump(4096, 2) != 9 || bump_peer(1) != large) return 6;
    if (bump(0, 0) != 8 || bump(4096, 0) != 11 || bump_peer(0) != large + 1) return 7;
    return dlclose(image);
}
'''

CONSUMER = '''
#include <string.h>

extern int score(int);
extern const char* greeting(void);

int main(void) {
    if (strcmp(greeting(), "native Mach-O leaf")) return 1;
    return score(35);
}
'''

NATIVE_CONSUMER = '''
extern "C" unsigned bump(unsigned, unsigned);
extern "C" unsigned long long bump_peer(unsigned long long);

int main() {
    const unsigned long long large = 1ULL << 40;
    if (bump(4096, 5) != 0 || bump(4096, 1) != 5) return 1;
    if (bump_peer(large) != 0 || bump_peer(0) != large) return 2;
    return 17;
}
'''

RULE = '''
#include "rule_api.h"

static volatile unsigned worker_invocations;

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) {
    if (!host || host->abi_version != YCXX_RULE_ABI_VERSION ||
        host->struct_size < sizeof(*host) || !host->attribute || !host->emit) return 1;
    ycxx_rule_bytes_v1 mode;
    if (host->attribute(host->context, "mode", &mode) != 1 ||
        mode.size != 1 || mode.data[0] != '7') return 2;
    if (worker_invocations != 0) return 3;
    worker_invocations = 1;
    if (worker_invocations != 1) return 4;
    const auto& plan = R"plan(native.executable "app" {
    sources = ["main.cc"]
})plan";
    return host->emit(host->context, plan, sizeof(plan) - 1);
}
'''
PLAN = b'native.executable "app" {\n    sources = ["main.cc"]\n}'

LC_SYMTAB = 0x2
LC_SEGMENT_64 = 0x19
LC_ID_DYLIB = 0xd
LC_LOAD_DYLIB = 0xc
LC_UUID = 0x1b
LC_CODE_SIGNATURE = 0x1d
LC_DYLD_INFO = 0x22
LC_DYLD_INFO_ONLY = 0x80000022
LC_DYLD_EXPORTS_TRIE = 0x80000033
LC_DYLD_CHAINED_FIXUPS = 0x80000034
VM_PAGE = 0x4000
PROVIDER_BSS = (16400, 3)  # 4,097 uint32_t slots, four bytes of padding, then one uint64_t.
WORKER_BSS = (4, 2)


def load_commands(data, file_type):
    magic, cpu, _, kind, count, size = struct.unpack_from('<6I', data)
    assert (magic, cpu, kind) == (0xfeedfacf, 0x100000c, file_type), (magic, cpu, kind)
    cursor, end = 32, 32 + size
    assert end <= len(data), (end, len(data))
    commands = []
    for _ in range(count):
        assert cursor + 8 <= end, (cursor, end)
        command, length = struct.unpack_from('<2I', data, cursor)
        assert length >= 8 and length % 8 == 0 and cursor + length <= end, (command, length)
        commands.append((command, data[cursor:cursor + length]))
        cursor += length
    assert cursor == end, (cursor, end)
    return commands


def dylib_name(command):
    offset, = struct.unpack_from('<I', command, 8)
    assert 24 <= offset < len(command), command
    return command[offset:command.index(b'\0', offset)].decode('utf-8')


class Section(NamedTuple):
    address: int
    size: int
    offset: int
    alignment: int
    relocations: int
    relocation_count: int
    flags: int


class Segment(NamedTuple):
    address: int
    vm_size: int
    offset: int
    file_size: int
    max_permissions: int
    permissions: int


def sections(commands):
    result = {}
    for command, payload in commands:
        if command != LC_SEGMENT_64:
            continue
        count, = struct.unpack_from('<I', payload, 64)
        for index in range(count):
            name, segment, *section = struct.unpack_from('<16s16s2Q5I', payload, 72 + 80 * index)
            key = segment.rstrip(b'\0'), name.rstrip(b'\0')
            assert key not in result, key
            result[key] = Section(*section)
    return result


def uleb(data, offset):
    result = 0
    for shift in range(0, 64, 7):
        value = data[offset]
        offset += 1
        result |= (value & 0x7f) << shift
        if value < 0x80:
            return result, offset
    raise AssertionError('overflowing export trie integer')


def exports(trie):
    def visit(offset, name, ancestors):
        assert offset not in ancestors, ('cyclic export trie', offset, name)
        size, cursor = uleb(trie, offset)
        terminal_end = cursor + size
        if size:
            flags, cursor = uleb(trie, cursor)
            address, cursor = uleb(trie, cursor)
            assert flags == 0 and cursor == terminal_end, (name, flags, size)
            yield name, address
        count = trie[terminal_end]
        cursor = terminal_end + 1
        for _ in range(count):
            end = trie.index(b'\0', cursor)
            edge = trie[cursor:end]
            child, cursor = uleb(trie, end + 1)
            yield from visit(child, name + edge, ancestors | {offset})

    return dict(visit(0, b'', set()))


def check_image(data, install_name, expected_exports, *, require_unwind, expected_bss):
    commands = load_commands(data, 6)  # MH_DYLIB
    command_ids = {command for command, _ in commands}
    # dyld requires the system dylib command even with no imported symbols.
    dependencies = [dylib_name(payload) for command, payload in commands if command == LC_LOAD_DYLIB]
    assert dependencies == ['/usr/lib/libSystem.B.dylib'], dependencies
    assert not command_ids & {0xe, 0x5, 0x20, 0x80000018, 0x8000001f, 0x80000023, 0x80000028}, command_ids
    names = [dylib_name(payload) for command, payload in commands if command == LC_ID_DYLIB]
    assert names == [install_name], names
    uuid, = (payload[8:] for command, payload in commands if command == LC_UUID)
    assert len(uuid) == 16 and any(uuid), uuid

    segments = {}
    for command, payload in commands:
        if command != LC_SEGMENT_64:
            continue
        name, *fields = struct.unpack_from('<16s4Q2I', payload, 8)
        name = name.rstrip(b'\0')
        segment = Segment(*fields)
        assert segment.file_size <= segment.vm_size and segment.offset + segment.file_size <= len(data), segment
        assert name not in segments, name
        if name == b'__TEXT':
            assert segment.address == segment.offset == 0, segment
        if name != b'__DATA':
            assert not (segment.max_permissions | segment.permissions) & 2, segment  # VM_PROT_WRITE
        segments[name] = segment
    assert list(segments) == [b'__TEXT', b'__DATA', b'__LINKEDIT'], segments
    linkedit = segments[b'__LINKEDIT']

    def linkedit_span(offset, length):
        assert linkedit.offset <= offset < offset + length <= linkedit.offset + linkedit.file_size, (
            offset, length, linkedit)
        return data[offset:offset + length]

    signature, = (struct.unpack_from('<2I', payload, 8) for command, payload in commands
                  if command == LC_CODE_SIGNATURE)
    linkedit_span(*signature)
    fixup_location, = (struct.unpack_from('<2I', payload, 8) for command, payload in commands
                       if command == LC_DYLD_CHAINED_FIXUPS)
    fixup_table = linkedit_span(*fixup_location)
    version, starts_offset, _, _, imports_count, _, _ = struct.unpack_from('<7I', fixup_table)
    assert version == 0 and starts_offset >= 28 and imports_count == 0, fixup_table
    segment_count, = struct.unpack_from('<I', fixup_table, starts_offset)
    assert segment_count == len(segments), (segment_count, segments)
    segment_starts = struct.unpack_from(f'<{segment_count}I', fixup_table, starts_offset + 4)
    assert not any(segment_starts), segment_starts

    export_ranges = []
    for command, payload in commands:
        if command in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY):
            *fixups, offset, length = struct.unpack_from('<10I', payload, 8)
            assert not any(fixups[1::2]), ('a leaf dylib requires runtime fixups', fixups)
            if length:
                export_ranges.append((offset, length))
        elif command == LC_DYLD_EXPORTS_TRIE:
            export_ranges.append(struct.unpack_from('<2I', payload, 8))
    assert export_ranges and len(set(export_ranges)) == 1, export_ranges
    symbols = exports(linkedit_span(*export_ranges[0]))
    assert symbols.keys() == expected_exports, symbols
    header_size = 32 + sum(len(payload) for _, payload in commands)
    text_segment = segments[b'__TEXT']
    assert all(header_size <= address < text_segment.file_size for address in symbols.values()), symbols

    named_sections = sections(commands)
    assert [name for segment, name in named_sections if segment == b'__DATA'] == [b'__bss'], named_sections
    zero_fill = named_sections[b'__DATA', b'__bss']
    assert (zero_fill.size, zero_fill.alignment) == expected_bss, zero_fill
    assert zero_fill.address % (1 << zero_fill.alignment) == 0, zero_fill
    assert (zero_fill.flags & 0xff, zero_fill.offset, zero_fill.relocations, zero_fill.relocation_count) == (
        1, 0, 0, 0), zero_fill  # S_ZEROFILL carries no stored bytes or relocations.
    writable = segments[b'__DATA']
    assert (writable.file_size, writable.max_permissions, writable.permissions) == (0, 3, 3), writable
    assert writable.address == zero_fill.address and writable.address % VM_PAGE == 0, writable
    assert writable.vm_size == ((zero_fill.size + VM_PAGE - 1) // VM_PAGE) * VM_PAGE, writable
    assert text_segment.address + text_segment.vm_size <= writable.address, segments
    assert writable.address + writable.vm_size <= linkedit.address, segments
    assert text_segment.offset + text_segment.file_size <= writable.offset <= linkedit.offset, segments

    text = named_sections[b'__TEXT', b'__text']
    frame = named_sections.get((b'__TEXT', b'__eh_frame'))
    unwind_ranges = []
    if frame:
        assert frame.relocation_count == 0, frame
        unwind_ranges = [(frame.address + field + displacement, extent)
                         for field, displacement, extent, _ in
                         records(data[frame.offset:frame.offset + frame.size], terminator=True)]
        assert all(text.address <= pc < pc + extent <= text.address + text.size
                   for pc, extent in unwind_ranges), (unwind_ranges, text)
    if require_unwind:
        assert unwind_ranges, 'ordinary unwind frames are missing from the linked provider'
    return {
        'architecture': 'arm64', 'type': 'MH_DYLIB', 'install_name': install_name,
        'dependencies': dependencies, 'uuid': uuid.hex(), 'commands': [hex(command) for command, _ in commands],
        'segments': {name.decode(): {**segment._asdict(), 'size': segment.file_size}
                     for name, segment in segments.items()},
        'zero_fill': {'segment': '__DATA', 'section': '__bss', **zero_fill._asdict()},
        'exports': {name.decode(): address for name, address in symbols.items()},
        'signature': {'offset': signature[0], 'size': signature[1]},
        'fixups': {'imports': imports_count, 'segment_starts': segment_starts},
        'unwind_ranges': unwind_ranges,
    }


def shifted_pageoff_add(data):
    changed = bytearray(data)
    text = sections(load_commands(data, 1))[b'__TEXT', b'__text']  # MH_OBJECT
    for index in range(text.relocation_count):
        address, info = struct.unpack_from('<iI', data, text.relocations + 8 * index)
        if info >> 28 != 4:  # ARM64_RELOC_PAGEOFF12
            continue
        location = text.offset + address
        instruction, = struct.unpack_from('<I', data, location)
        if instruction & 0xffc00000 == 0x91000000:
            struct.pack_into('<I', changed, location, instruction | (1 << 22))
            return bytes(changed)
    raise AssertionError('the provider object has no unshifted ADD with a PAGEOFF12 relocation')


def malformed_unwind(data):
    frame = sections(load_commands(data, 1))[b'__TEXT', b'__eh_frame']
    augmentation = frame.offset + 9  # after the initial CIE length, ID, and version
    assert data[augmentation:augmentation + 3] == b'zR\0', frame
    return data[:augmentation + 1] + b'P' + data[augmentation + 2:]


def absolute_text_relocation(data):
    text = sections(load_commands(data, 1))[b'__TEXT', b'__text']
    for index in range(text.relocation_count):
        offset = text.relocations + 8 * index + 4
        info, = struct.unpack_from('<I', data, offset)
        if info >> 28 == 3:  # Replace PAGE21 with a 64-bit ARM64_RELOC_UNSIGNED.
            info = (info & 0x08ffffff) | (3 << 25)
            return data[:offset] + struct.pack('<I', info) + data[offset + 4:]
    raise AssertionError('the provider object has no PAGE21 relocation')


def branch_to_zero_fill(data):
    commands = load_commands(data, 1)
    named_sections = sections(commands)
    bss_index = list(named_sections).index((b'__DATA', b'__bss')) + 1  # n_sect is one-based.
    symbols, count = next(struct.unpack_from('<2I', payload, 8)
                          for command, payload in commands if command == LC_SYMTAB)
    target = next(i for i in range(count) if data[symbols + 16 * i + 5] == bss_index)
    text = named_sections[b'__TEXT', b'__text']
    for index in range(text.relocation_count):
        offset = text.relocations + 8 * index + 4
        info, = struct.unpack_from('<I', data, offset)
        if info >> 28 == 2:  # ARM64_RELOC_BRANCH26: retain the branch and redirect its symbol index.
            info = (info & 0xff000000) | 0x08000000 | target  # Set r_extern for the nlist index.
            return data[:offset] + struct.pack('<I', info) + data[offset + 4:]
    raise AssertionError('the provider object has no BRANCH26 relocation')


def main():
    ycxx, sdk, *darwin_tools = sys.argv[1:]
    target = '--target=arm64-apple-darwin'
    install_name = '@loader_path/installed/libycxx-leaf.dylib'
    shared = (target, '-shared', '-fPIC', '-nostdlib')
    staged = Path('staging/provider.dylib')
    exports_expected = {b'_score', b'_greeting', b'_bump', b'_bump_peer'}
    receipt_path = os.environ.get('YCXX_DARWIN_SHARED_RECEIPTS')
    receipts = Path(receipt_path).resolve() if receipt_path else None
    command_number = 0
    if receipts is not None:
        receipts.mkdir(parents=True, exist_ok=True)
        (receipts / 'commands.jsonl').write_text('', encoding='utf-8')

    def retain(name, data):
        if receipts is not None:
            (receipts / name).write_bytes(data)

    with (
        tempfile.TemporaryDirectory(prefix='ycxx-darwin-shared-', dir='.') as directory,
        tempfile.TemporaryDirectory(prefix='yd-', dir='/tmp') as runtime,
    ):
        scope = Path(directory).resolve()
        (scope / '.ycxx').touch()
        for subdir in ('staging', 'installed', 'worker'):
            (scope / subdir).mkdir()
        base = {key: value for key, value in os.environ.items() if key not in
                {'YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT'} and not key.startswith('DYLD_')}
        base.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')

        def run(*args, status=0, env=base, timeout=60):
            nonlocal command_number
            result = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=receipts is None,
                                    timeout=timeout)
            if receipts is not None:
                command_number += 1
                prefix = f'command-{command_number:02d}'
                stdout, stderr = prefix + '.stdout', prefix + '.stderr'
                retain(stdout, result.stdout)
                retain(stderr, result.stderr)
                outcome = {'argv': [str(arg) for arg in args], 'cwd': str(scope), 'status': result.returncode,
                           'expected_status': status, 'stdout': stdout, 'stderr': stderr}
                with (receipts / 'commands.jsonl').open('a', encoding='utf-8') as journal:
                    journal.write(json.dumps(outcome) + '\n')
                result.stdout = result.stdout.decode('utf-8', errors='replace')
                result.stderr = result.stderr.decode('utf-8', errors='replace')
            if status is not None:
                assert result.returncode == status, (args, result.returncode, status, result.stdout, result.stderr)
            return result

        def cold(*args, **kwargs):
            return run(ycxx, 'cxx', '--no-daemon', *args, **kwargs)

        def write(path, text):
            (scope / path).write_text(text, encoding='utf-8')

        def provider(bias):
            write('provider.cpp', PROVIDER.replace('BIAS', str(bias)))

        def verify_image(path, label, name, expected=exports_expected, *,
                         require_unwind=True, expected_bss=PROVIDER_BSS):
            data = (scope / path).read_bytes()
            retain(label + '.dylib', data)
            metadata = check_image(data, name, expected, require_unwind=require_unwind, expected_bss=expected_bss)
            retain(label + '.json', (json.dumps(metadata, indent=2) + '\n').encode('utf-8'))
            return data

        def arguments(name):
            return (*shared, f'-Wl,-install_name,{name}', 'provider.cpp', 'peer.cpp', '-o', staged)

        def traced(*args, kind, outcome):
            result = run(ycxx, 'cxx', '--daemon=require', '--trace-engine=json', *args)
            records = [json.loads(line) for line in result.stderr.splitlines() if line.strip()]
            assert any(row.get('kind') == kind and row.get('outcome') == outcome for row in records), records
            return records

        provider(7)
        write('peer.cpp', PEER)
        args = arguments(install_name)
        cold(*args, env=dict(base, PATH=''))
        first = verify_image(staged, 'provider-cold', install_name)

        cold(target, '-fPIC', '-c', 'provider.cpp', '-o', 'provider.o', env=dict(base, PATH=''))
        cold(target, '-fPIC', '-c', 'peer.cpp', '-o', 'peer.o', env=dict(base, PATH=''))
        cold(target, '-dynamiclib', '-nostdlib', 'provider.o', 'peer.o', '-o', 'staging/default.dylib',
             env=dict(base, PATH=''))
        verify_image('staging/default.dylib', 'provider-object-input', 'default.dylib')
        provider_object = (scope / 'provider.o').read_bytes()
        for name, mutate, diagnostic in (
            ('shifted-add.o', shifted_pageoff_add, 'PAGEOFF12 dylib relocation requires an unshifted ADD immediate'),
            ('malformed-unwind.o', malformed_unwind, 'Mach-O leaf dylib requires a single ordinary zR unwind section'),
            ('absolute-text.o', absolute_text_relocation, 'unsupported relocation for Mach-O leaf dylib output in __text'),
            ('branch-to-zero-fill.o', branch_to_zero_fill, 'BRANCH26 target is in non-executable zero-fill storage'),
        ):
            malformed = mutate(provider_object)
            (scope / name).write_bytes(malformed)
            retain(name, malformed)
            rejected = cold(*shared, name, '-o', 'rejected.dylib', status=1)
            assert diagnostic in rejected.stderr, rejected.stderr
            assert not (scope / 'rejected.dylib').exists()

        write('native-consumer.cpp', NATIVE_CONSUMER)
        cold(target, '-nostdlib++', 'native-consumer.cpp', staged, '-o', 'native-client')
        native_client = (scope / 'native-client').read_bytes()
        retain('native-client', native_client)
        dependencies = [dylib_name(payload) for command, payload in load_commands(native_client, 2)
                        if command == LC_LOAD_DYLIB]
        assert dependencies == ['/usr/lib/libSystem.B.dylib', install_name], dependencies

        # PARITY: by design: only native arm64 macOS can use the platform linker and run dyld.
        if darwin_tools:
            cc, codesign = darwin_tools
            run(codesign, '--verify', '--strict', staged)
            if receipts is not None:
                run(codesign, '-d', '--verbose=4', staged)
                run('/usr/bin/otool', '-l', staged)
            write('loader.c', LOADER)
            run(cc, '-arch', 'arm64', 'loader.c', '-o', 'loader')
            run('./loader', str(scope / staged), '42')
            write('consumer.c', CONSUMER)
            run(cc, '-arch', 'arm64', 'consumer.c', staged, '-o', 'system-client')
            dependencies = [dylib_name(payload) for command, payload in
                            load_commands((scope / 'system-client').read_bytes(), 2) if command == LC_LOAD_DYLIB]
            assert install_name in dependencies, dependencies
            if receipts is not None:
                run('/usr/bin/otool', '-L', 'system-client')
            (scope / staged).rename(scope / install_name.removeprefix('@loader_path/'))
            run('./system-client', status=42)
            run(codesign, '--verify', '--strict', 'native-client')
            run('./native-client', status=17)

            # Exercise the shipped SDK and the same loader/entry point the coordinator invokes.
            write('worker/provider.cpp', RULE)
            cold(*shared, '-nostdinc++', '-ffreestanding', '-I' + sdk,
                 'worker/provider.cpp', '-o', 'worker/provider.dylib')
            verify_image('worker/provider.dylib', 'worker-provider', 'provider.dylib', {b'_ycxx_rule_plan_v1'},
                         require_unwind=False, expected_bss=WORKER_BSS)
            (scope / 'worker/provider.dylib').rename(scope / 'worker/rules.so')
            run(codesign, '--verify', '--strict', 'worker/rules.so')
            write('worker/request.json', json.dumps({
                'version': 1, 'target': 'app', 'rule': 'executable', 'definition': '{}',
                'attributes': [{'name': 'mode', 'value': '7'}], 'reads': [],
                'capabilities': None, 'witness': None, 'facts': None,
            }))
            run(ycxx, '--internal-rule-worker', 'worker/rules.so', 'worker/request.json', 'worker/plan.ycxx')
            plan = (scope / 'worker/plan.ycxx').read_bytes()
            retain('worker-plan.ycxx', plan)
            assert plan == PLAN

        for body, reason in (
            ('extern "C" int absent(); extern "C" int f() { return absent(); }', r'undefined symbol _absent\b'),
            ('static int writable = 7; extern "C" int f() { return ++writable; }',
             r'unsupported Mach-O dylib section __DATA,__data'),
            ('extern "C" { unsigned public_zero_fill; } extern "C" int f() { return 1; }',
             r'Mach-O dylib export _public_zero_fill must be a strong function in executable code'),
        ):
            write('rejected.cpp', body + '\n')
            cold(target, '-fPIC', '-c', 'rejected.cpp', '-o', 'rejected.o')
            rejected = cold(*shared, 'rejected.o', '-o', 'rejected.dylib', status=1)
            assert re.search(reason, rejected.stderr, re.I), rejected.stderr
            assert not (scope / 'rejected.dylib').exists()

        try:
            traced(*args, kind='Link', outcome='executed')
            resident = (scope / staged).read_bytes()
            retain('provider-daemon.dylib', resident)
            assert resident == first
            warm = traced(*args, kind='Build', outcome='reused')
            assert not any(row.get('kind') == 'Link' and row.get('outcome') == 'executed' for row in warm), warm
            assert (scope / staged).read_bytes() == first
            provider(8)
            traced(*args, kind='Link', outcome='executed')
            changed = verify_image(staged, 'provider-edited', install_name)
            assert changed != first
            if darwin_tools:
                run(codesign, '--verify', '--strict', staged)
                run('./loader', str(scope / staged), '43')
            cold(*args)
            assert (scope / staged).read_bytes() == changed

            other_name = str(scope / 'installed/absolute.dylib')
            traced(*arguments(other_name), kind='Link', outcome='executed')
            other = verify_image(staged, 'provider-absolute-install-name', other_name)
            assert other != changed
        finally:
            run(ycxx, 'ctl', 'shutdown', status=None, timeout=10)


if __name__ == '__main__':
    main()
