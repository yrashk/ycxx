"""Cast through peer-owned RTTI and replay changes to key ownership."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from linux_compile import run
from linux_peer_derived_default_construction import native_compilers

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'daemon'))
from runtime_dir import runtime_root


def check_imports(readelf, scope, env, obj, target_class):
    metadata = run([readelf, '-Wsr', obj], scope, env)
    rows = [line.split() for line in metadata.splitlines()]
    symbols = [row for row in rows if len(row) >= 8 and row[0][:-1].isdigit() and row[0].endswith(':')]
    relocations = [row for row in rows if len(row) >= 5 and row[2].startswith('R_X86_64_')]
    for name in ('_ZTI1B', f'_ZTI{target_class}', '__dynamic_cast'):
        imports = [row for row in symbols if row[7] == name]
        assert len(imports) == 1 and imports[0][4:7] == ['GLOBAL', 'DEFAULT', 'UND'], metadata
        index = int(imports[0][0][:-1])
        assert any(row[4] == name and int(row[1], 16) >> 32 == index for row in relocations), metadata
    forbidden = {f'{prefix}{name}' for prefix in ('_ZTV', '_ZTS') for name in ('1B', '1D', '7Renamed')}
    forbidden.add('_ZTI7Renamed' if target_class == '1D' else '_ZTI1D')
    assert not any(row[7] in forbidden for row in symbols), metadata


def main():
    ycxx, host, readelf = sys.argv[1:]
    fixture = Path(__file__).resolve().with_suffix('')
    target = [ycxx, 'cxx', '--target=x86_64-pc-linux-gnu']
    with tempfile.TemporaryDirectory(prefix='ycxx-borrowed-downcast-') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-', dir=runtime_root('yd-')) as runtime:
        scope = Path(temp).resolve()
        (scope / '.ycxx').touch()
        inputs = {scope / name: (fixture / name).read_bytes()
                  for name in ('classes.hpp', 'consumer.cpp', 'peer.cpp')}
        for path, content in inputs.items():
            path.write_bytes(content)
        header = scope / 'classes.hpp'
        consumer = scope / 'consumer.cpp'
        base = {key: value for key, value in os.environ.items() if key not in
                ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT', 'YCXX_NO_WATCH')}
        base.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2', XDG_CACHE_HOME=str(scope / 'cache'))
        cold = dict(base, YCXX_NO_DAEMON='1')
        resident = dict(base, YCXX_DAEMON='require')
        peers = native_compilers(host, scope, cold)
        syntax = [*target, '-fsyntax-only', consumer.name]
        object_command = [*target, '-c', consumer.name, '-o']

        def daemon_pid():
            status = run([ycxx, 'ctl', 'status'], scope, base)
            pid, = (line.removeprefix('pid: ') for line in status.splitlines() if line.startswith('pid: '))
            return int(pid)

        def build_both(target_class):
            run([*object_command, 'cold.o'], scope, cold)
            run([*object_command, 'consumer.o'], scope, resident)
            assert daemon_pid() == pid, 'daemon PID changed during object compilation'
            obj = (scope / 'consumer.o').read_bytes()
            assert obj == (scope / 'cold.o').read_bytes(), 'resident and cold object bytes differ'
            check_imports(readelf, scope, cold, 'consumer.o', target_class)
            return obj

        def run_peers():
            for compiler in peers:
                run([compiler, '-std=c++20', '-pedantic-errors', '-O0', '-frtti', '-fno-lto', '-fno-pie',
                     '-c', 'peer.cpp', '-o', 'peer.o'], scope, cold)
                run([compiler, '-no-pie', '-Wl,--wrap=__dynamic_cast', 'consumer.o', 'peer.o', '-o', 'mixed'],
                    scope, cold)
                run(['./mixed'], scope, cold)

        def reject_object(diagnostic_fragment=''):
            diagnostics = []
            for env in (cold, resident):
                run(syntax, scope, env)
                result = subprocess.run([*object_command, 'rejected.o'], cwd=scope, env=env,
                                        capture_output=True, text=True, timeout=30)
                errors = [line for line in result.stderr.splitlines() if 'error:' in line]
                assert result.returncode > 0 and any(diagnostic_fragment in line for line in errors), result
                assert not (scope / 'rejected.o').exists(), 'unsupported cast produced an ELF object'
                diagnostics.append(result.stderr)
            assert diagnostics[0] == diagnostics[1], diagnostics
            assert daemon_pid() == pid, 'daemon PID changed during rejection'

        try:
            run(syntax, scope, resident)
            pid = daemon_pid()
            original = build_both('1D')
            run_peers()

            override = b'    int f() override;\n'
            assert inputs[header].count(override) == 1
            header.write_bytes(inputs[header].replace(override, b''))
            reject_object('dynamic_cast')
            header.write_bytes(inputs[header])
            assert build_both('1D') == original, 'restoring the override changed the object'

            consumer.write_bytes(inputs[consumer] + b'\nint D::f() { return 3; }\n')
            reject_object()
            consumer.write_bytes(inputs[consumer])
            assert build_both('1D') == original, 'restoring the borrowed key changed the object'

            for path, content in inputs.items():
                path.write_bytes(re.sub(rb'\bD\b', b'Renamed', content))
            assert build_both('7Renamed') != original
            run_peers()
            for path, content in inputs.items():
                path.write_bytes(content)
            assert build_both('1D') == original, 'restoring the class name changed the object'
        finally:
            subprocess.run([ycxx, 'ctl', 'shutdown'], cwd=scope, env=base, capture_output=True, timeout=30)


if __name__ == '__main__':
    main()
