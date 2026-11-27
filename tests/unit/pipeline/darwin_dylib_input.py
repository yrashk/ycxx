"""Consume explicit arm64 Mach-O leaf dylibs in native executable links (#392)."""

import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from darwin_shared_output import (
    LC_CODE_SIGNATURE, LC_DYLD_CHAINED_FIXUPS, LC_DYLD_EXPORTS_TRIE, LC_ID_DYLIB, LC_LOAD_DYLIB, LC_UUID,
    dylib_name, load_commands,
)


TARGET = '--target=arm64-apple-darwin'
LIBSYSTEM = '/usr/lib/libSystem.B.dylib'
NATIVE_ID = '@loader_path/installed/libfixture-native.dylib'
PLATFORM_ID = '@loader_path/installed/libfixture-platform.dylib'
NATIVE_PATH = Path('staged/native-input.dylib')
PLATFORM_PATH = Path('staged/platform-input.dylib')
CLIENT = '''
extern "C" int native_step(int, int);
extern "C" int platform_step(int, int);
int main() { return native_step(11, 7) + platform_step(2, 3); }
'''


def library(command):
    current, compatibility = struct.unpack_from('<2I', command, 16)
    return {'install_name': dylib_name(command), 'current_version': current,
            'compatibility_version': compatibility}


def identity(data, install_name):
    metadata, = (library(payload) for command, payload in load_commands(data, 6) if command == LC_ID_DYLIB)
    assert metadata['install_name'] == install_name, metadata
    return metadata


def executable_metadata(data, providers):
    commands = load_commands(data, 2)  # MH_EXECUTE
    dependencies = [library(payload) for command, payload in commands if command == LC_LOAD_DYLIB]
    names = [entry['install_name'] for entry in dependencies]
    assert names[:3] == [LIBSYSTEM, NATIVE_ID, PLATFORM_ID], names
    assert dependencies[1:3] == providers, dependencies

    fixups, = (struct.unpack_from('<2I', payload, 8)
               for command, payload in commands if command == LC_DYLD_CHAINED_FIXUPS)
    offset, size = fixups
    assert 0 < offset < offset + size <= len(data), fixups
    table = data[offset:offset + size]
    version, _, import_offset, symbol_offset, count, import_format, symbol_format = struct.unpack_from('<7I', table)
    # DYLD_CHAINED_IMPORT encodes a library ordinal, weak bit and symbol pool offset.
    assert (version, import_format, symbol_format) == (0, 1, 0), fixups
    assert 28 <= import_offset <= import_offset + count * 4 <= symbol_offset < len(table), fixups
    imports = {}
    for entry in struct.unpack_from(f'<{count}I', table, import_offset):
        ordinal = entry & 0xff
        weak = bool(entry & 0x100)
        start = symbol_offset + (entry >> 9)
        symbol = table[start:table.index(b'\0', start)].decode()
        assert symbol not in imports, (symbol, imports)
        imports[symbol] = {'ordinal': ordinal, 'weak': weak,
                           'install_name': names[ordinal - 1] if 0 < ordinal <= len(names) else None}
    for symbol, ordinal, install_name in (('_native_step', 2, NATIVE_ID), ('_platform_step', 3, PLATFORM_ID)):
        assert imports.get(symbol) == {'ordinal': ordinal, 'weak': False, 'install_name': install_name}, imports
    signature, = (struct.unpack_from('<2I', payload, 8)
                  for command, payload in commands if command == LC_CODE_SIGNATURE)
    assert 0 < signature[0] < sum(signature) <= len(data), signature
    return {'architecture': 'arm64', 'type': 'MH_EXECUTE', 'dependencies': dependencies, 'imports': imports}


def dylib_command(data, kind):
    offset = 32
    for command, payload in load_commands(data, 6):
        if command == kind:
            return offset, payload
        offset += len(payload)
    raise AssertionError(f'the provider has no load command {kind:#x}')


def malformed_install_name(data):
    offset, payload = dylib_command(data, LC_ID_DYLIB)
    # Point the command's name after its last byte.
    return data[:offset + 8] + struct.pack('<I', len(payload)) + data[offset + 12:]


def malformed_export_trie(data):
    offset, _ = dylib_command(data, LC_DYLD_EXPORTS_TRIE)
    _, size = struct.unpack_from('<2I', data, offset + 8)
    assert size > 0
    return data[:offset + 8] + struct.pack('<I', len(data)) + data[offset + 12:]


def unsupported_required_command(data):
    offset, _ = dylib_command(data, LC_UUID)
    return data[:offset] + struct.pack('<I', 0x8000007e) + data[offset + 4:]


def unsupported_dependency(data):
    offset, dependency = dylib_command(data, LC_LOAD_DYLIB)
    assert dylib_name(dependency) == LIBSYSTEM
    name_offset, = struct.unpack_from('<I', dependency, 8)
    letter = offset + name_offset + LIBSYSTEM.index('S')
    return data[:letter] + b'X' + data[letter + 1:]


def main():
    ycxx, *darwin_tools = sys.argv[1:]
    receipt_path = os.environ.get('YCXX_DARWIN_DYLIB_INPUT_RECEIPTS')
    receipts = Path(receipt_path).resolve() if receipt_path else None
    if receipts is not None:
        receipts.mkdir(parents=True, exist_ok=True)
        (receipts / 'commands.jsonl').write_text('', encoding='utf-8')

    def retain(name, data):
        if receipts is not None:
            (receipts / name).write_bytes(data)

    def retain_json(name, metadata):
        retain(name, (json.dumps(metadata, indent=2) + '\n').encode())

    command_number = 0
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-darwin-dylib-input-', dir='.') as directory,
        tempfile.TemporaryDirectory(prefix='ydi-', dir='/tmp') as runtime,
    ):
        scope = Path(directory).resolve()
        (scope / '.ycxx').touch()
        for subdir in ('staged', 'installed', 'replacement'):
            (scope / subdir).mkdir()
        env = {key: value for key, value in os.environ.items() if key not in
               {'YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT'} and not key.startswith('DYLD_')}
        env.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')

        def run(*args, status=0, timeout=60):
            nonlocal command_number
            result = subprocess.run(args, cwd=scope, env=env, capture_output=True, timeout=timeout)
            if receipts is not None:
                command_number += 1
                prefix = f'command-{command_number:02d}'
                stdout, stderr = prefix + '.stdout', prefix + '.stderr'
                retain(stdout, result.stdout)
                retain(stderr, result.stderr)
                evidence = {'argv': [str(arg) for arg in args], 'status': result.returncode,
                            'expected_status': status, 'cwd': str(scope), 'stdout': stdout, 'stderr': stderr}
                with (receipts / 'commands.jsonl').open('a', encoding='utf-8') as journal:
                    journal.write(json.dumps(evidence) + '\n')
            assert status is None or result.returncode == status, (args, status, result)
            return result.stderr.decode(errors='replace')

        def cold(*args, **kwargs):
            return run(ycxx, 'cxx', '--no-daemon', *args, **kwargs)

        def traced(*args, kind, outcome):
            stderr = run(ycxx, 'cxx', '--daemon=require', '--trace-engine=json', *args)
            events = [json.loads(line) for line in stderr.splitlines() if line.strip()]
            assert any(row.get('kind') == kind and row.get('outcome') == outcome for row in events), events
            return events

        def write(path, body):
            (scope / path).write_text(body, encoding='utf-8')

        def build_provider(install_name, source, output, *flags):
            cold(TARGET, '-shared', '-fPIC', '-nostdlib', f'-Wl,-install_name,{install_name}',
                 *flags, source, '-o', output)

        write('native.cpp', 'extern "C" int native_step(int a, int b) { return a + 2 * b + BIAS; }\n')
        write('platform.c', 'int platform_step(int a, int b) { return 3 * a + b; }\n')
        write('client.cpp', CLIENT)
        build_provider(NATIVE_ID, 'native.cpp', NATIVE_PATH, '-DBIAS=7')
        native = (scope / NATIVE_PATH).read_bytes()
        retain('provider-native.dylib', native)

        # PARITY: by design: the platform linker, signature verification and dyld require native arm64 macOS.
        if darwin_tools:
            cc, codesign = darwin_tools
            run(cc, '-arch', 'arm64', '-dynamiclib', '-nostdlib', '-lSystem',
                f'-Wl,-install_name,{PLATFORM_ID}', '-Wl,-current_version,3.2.1',
                '-Wl,-compatibility_version,2.1.0', 'platform.c', '-o', PLATFORM_PATH)
        else:
            write('platform.cpp', 'extern "C" int platform_step(int a, int b) { return 3 * a + b; }\n')
            build_provider(PLATFORM_ID, 'platform.cpp', PLATFORM_PATH)
        platform = (scope / PLATFORM_PATH).read_bytes()
        retain('provider-platform.dylib', platform)
        providers = [identity(native, NATIVE_ID), identity(platform, PLATFORM_ID)]
        if darwin_tools:
            assert providers[1]['current_version'] == 0x030201, providers
            assert providers[1]['compatibility_version'] == 0x020100, providers
        retain_json('provider-identities.json', providers)

        args = (TARGET, '-nostdlib++', 'client.cpp', NATIVE_PATH, PLATFORM_PATH, '-o', 'client')

        def check_client(path, label):
            data = (scope / path).read_bytes()
            retain(label, data)
            retain_json(label + '.json', executable_metadata(data, providers))
            return data

        def run_installed(client, status):
            installed = [(scope / source, scope / install_name.removeprefix('@loader_path/'))
                         for source, install_name in ((NATIVE_PATH, NATIVE_ID), (PLATFORM_PATH, PLATFORM_ID))]
            for source, destination in installed:
                source.rename(destination)
            try:
                for _, destination in installed:
                    run(codesign, '--verify', '--strict', destination)
                run(codesign, '--verify', '--strict', client)
                run(f'./{client}', status=status)
                if receipts is not None:
                    run('/usr/bin/otool', '-L', client)
            finally:
                for source, destination in installed:
                    destination.rename(source)

        cold(*args)
        first = check_client('client', 'client-cold')
        if darwin_tools:
            run_installed('client', 41)
            cold(TARGET, 'client.cpp', NATIVE_PATH, PLATFORM_PATH, '-o', 'client-runtime')
            check_client('client-runtime', 'client-runtime')
            run_installed('client-runtime', 41)

        try:
            traced(*args, kind='Link', outcome='executed')
            assert check_client('client', 'client-daemon') == first
            warm = traced(*args, kind='Build', outcome='reused')
            assert not any(row.get('kind') == 'Link' and row.get('outcome') == 'executed' for row in warm), warm
            assert (scope / 'client').read_bytes() == first

            pending = scope / 'replacement' / NATIVE_PATH.name
            build_provider(NATIVE_ID, 'native.cpp', pending, '-DBIAS=8')
            replacement = pending.read_bytes()
            assert replacement != native and len(replacement) == len(native)
            assert identity(replacement, NATIVE_ID) == providers[0]
            retain('provider-native-replaced.dylib', replacement)
            old = (scope / NATIVE_PATH).stat()
            os.utime(pending, ns=(old.st_atime_ns, old.st_mtime_ns))
            pending.replace(scope / NATIVE_PATH)
            new = (scope / NATIVE_PATH).stat()
            assert (new.st_size, new.st_mtime_ns) == (old.st_size, old.st_mtime_ns)

            traced(*args, kind='Link', outcome='executed')
            changed = check_client('client', 'client-after-replacement')
            cold(*args)
            assert (scope / 'client').read_bytes() == changed
            if darwin_tools:
                run_installed('client', 42)
        finally:
            run(ycxx, 'ctl', 'shutdown', status=None, timeout=10)

        def reject(*args, diagnostic):
            stderr = cold(*args, '-o', 'rejected', status=1)
            assert diagnostic in stderr, stderr
            assert not (scope / 'rejected').exists()
            return stderr

        write('unknown.cpp', 'extern "C" int fixture_missing_export();\n'
              'int main() { return fixture_missing_export(); }\n')
        reject(TARGET, '-nostdlib++', 'unknown.cpp', NATIVE_PATH,
               diagnostic='undefined symbol _fixture_missing_export')
        reject(TARGET, '-nostdlib++', 'client.cpp', 'absent.dylib', diagnostic='absent.dylib')
        reject(TARGET, '-nostdlib++', 'client.cpp', NATIVE_PATH, NATIVE_PATH,
               diagnostic='duplicate Mach-O dylib install name')
        reject('--target=x86_64-pc-linux-gnu', '-nostdlib++', 'client.cpp', NATIVE_PATH,
               diagnostic='Mach-O dylib inputs require an arm64 Darwin executable link')
        reject(TARGET, '-c', 'client.cpp', NATIVE_PATH,
               diagnostic='Mach-O dylib inputs require an arm64 Darwin executable link')
        reject(TARGET, '-shared', '-nostdlib', 'client.cpp', NATIVE_PATH,
               diagnostic='does not support archive or shared object inputs')

        for path, data, diagnostic in (
            ('wrong-type.dylib', first, 'expected a thin arm64 Mach-O dylib'),
            ('bad-install-name.dylib', malformed_install_name(native), 'Mach-O dylib install name'),
            ('bad-export-trie.dylib', malformed_export_trie(native), 'Mach-O dylib export trie'),
            ('required-command.dylib', unsupported_required_command(native),
             'unsupported required Mach-O dylib load command'),
            ('unsupported-dependency.dylib', unsupported_dependency(native), 'unsupported Mach-O dylib dependency'),
        ):
            (scope / path).write_bytes(data)
            retain(path, data)
            stderr = reject(TARGET, '-nostdlib++', 'client.cpp', path, diagnostic=diagnostic)
            assert path in stderr, stderr


if __name__ == '__main__':
    main()
