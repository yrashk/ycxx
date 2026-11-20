"""Run declaration-owned closures through the Linux compiler and the resident daemon."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    ycxx, readelf = sys.argv[1:]
    source_dir = Path(__file__).with_name('namespace-lambda')
    target = '--target=x86_64-pc-linux-gnu'
    with tempfile.TemporaryDirectory(prefix='ycxx-namespace-lambda-') as temp, \
            tempfile.TemporaryDirectory(prefix='yd-') as runtime:
        scope = Path(temp)
        (scope / '.ycxx').touch()
        for source in source_dir.iterdir():
            shutil.copyfile(source, scope / source.name)
        base = {k: v for k, v in os.environ.items() if k not in ('YCXX_NO_DAEMON', 'YCXX_DAEMON', 'YCXX_ROOT')}
        base.update(LC_ALL='C', YCXX_RUNTIME_DIR=runtime, YCXX_THREADS='2')
        cold = dict(base, YCXX_NO_DAEMON='1')
        daemon = dict(base, YCXX_DAEMON='require')

        def run(args, status=0, error='', env=cold):
            result = subprocess.run(args, cwd=scope, env=env, capture_output=True, text=True, timeout=45)
            if result.returncode != status or error not in result.stderr:
                raise AssertionError(f'{args} exited {result.returncode}, expected {status}'
                                     f' and stderr containing {error!r}\n{result.stdout}{result.stderr}')
            return result

        def compile(*args, flags=(target,), **kwargs):
            return run([ycxx, 'cxx', *flags, *args], **kwargs)

        def native(*inputs, status=0, env=cold):
            compile('-nostdlib', *inputs, '-o', 'program', env=env)
            run(['./program'], status=status)

        def symbols(path):
            return {line.split()[-1]: line.split()[4] for line in run([readelf, '-Ws', path]).stdout.splitlines()
                    if line.lstrip().split(':', 1)[0].isdigit() and len(line.split()) == 8}

        for name in ('runtime', 'constinit', 'constexpr', 'invoker'):
            native(f'{name}.cpp')
        compile('-fsyntax-only', 'scope.cpp')
        # Internal referents keep the native probe independent of external ELF data addressing (#414).
        scoped = (scope / 'scope.cpp').read_text().replace('{ int a=', '{ static int a=')
        (scope / 'static-scope.cpp').write_text(scoped)
        native('static-scope.cpp')
        compile('-c', 'static-scope.cpp', '-o', 'scope.o')
        ns_symbols = symbols('scope.o')
        for name in ('A', 'B'):
            assert ns_symbols.get(f'_ZNK1{name}1fMUliE_clEi') == 'LOCAL', ns_symbols

        compile('-c', 'constinit.cpp')
        sections = run([readelf, '-SW', 'constinit.o']).stdout
        assert '.bss' in sections and '.init_array' not in sections, sections
        const_symbols = symbols('constinit.o')
        assert const_symbols.get('triple') == 'LOCAL' and const_symbols.get('other') == 'LOCAL', const_symbols
        assert const_symbols.get('_ZNK6tripleMUliE_clEi') == 'LOCAL', const_symbols
        assert const_symbols.get('_ZN6tripleMUliE_8__invokeEi') == 'LOCAL', const_symbols
        compile('-c', 'constinit.cpp', '-o', 'darwin.o', flags=())
        assert (scope / 'darwin.o').read_bytes().startswith(bytes.fromhex('cffaedfe'))

        for name in ('left', 'right', 'driver'):
            compile('-c', f'{name}.cpp')
        for path in ('left.o', 'right.o'):
            assert symbols(path).get('_ZNK1fMUliE_clEi') == 'LOCAL'
        native('driver.o', 'left.o', 'right.o')
        native('driver.o', 'right.o', 'left.o')

        for name, error in (('capture_default', 'namespace-scope lambda cannot have a capture-default'),
                            ('self', 'is used in its own initializer'),
                            ('init_capture', 'init-capture in a namespace-scope lambda is not supported yet')):
            compile('-fsyntax-only', f'{name}.cpp', status=1, error=error)
        (scope / 'qualified-self.cpp').write_text('namespace N { static auto f=[](){return N::f();}; }\n')
        compile('-fsyntax-only', 'qualified-self.cpp', status=1, error='is used in its own initializer')

        edit = scope / 'edit.cpp'

        def check_edit(name, status):
            compile('-c', 'edit.cpp', '-o', 'cold.o')
            compile('-c', 'edit.cpp', '-o', 'warm.o', env=daemon)
            assert (scope / 'cold.o').read_bytes() == (scope / 'warm.o').read_bytes()
            assert symbols('warm.o').get(f'_ZNK1{name}MUliE_clEi') == 'LOCAL'
            native('edit.cpp', status=status, env=daemon)
            native('edit.cpp', status=status)

        def daemon_status():
            status = run([ycxx, 'ctl', 'status'], env=base).stdout
            return {k: v for k, v in (line.split(': ', 1) for line in status.splitlines() if ': ' in line)}

        def edit_in_place(name, term):
            edit.write_text(f'static auto {name}=[](int x){{return {term};}}; int main(){{return {name}(10);}}\n')

        edit_in_place('f', 'x+1')
        try:
            check_edit('f', 11)
            pid = daemon_status()['pid']
            compile('-c', 'edit.cpp', '-o', 'warm.o', env=daemon)
            compile('-c', 'edit.cpp', '-o', 'warm.o', env=daemon)
            assert daemon_status()['last_executed'] == '0'
            edit_in_place('f', 'x+2')
            check_edit('f', 12)
            edit_in_place('f', 'f(x)')
            rejected = [
                compile(
                    '-c', 'edit.cpp', '-o', 'rejected.o', env=mode, status=1,
                    error='is used in its own initializer').stderr
                for mode in (cold, daemon)
            ]
            assert rejected[0] == rejected[1] and not (scope / 'rejected.o').exists()
            edit_in_place('f', 'x+2')
            check_edit('f', 12)
            edit_in_place('g', 'x+2')
            check_edit('g', 12)
            assert '_ZNK1fMUliE_clEi' not in symbols('warm.o')
            edit.write_text('static auto h{[](int x){return x+3;}};\n' + edit.read_text())
            check_edit('g', 12)
            assert symbols('warm.o').get('_ZNK1hMUliE_clEi') == 'LOCAL'
            assert daemon_status()['pid'] == pid
        finally:
            run([ycxx, 'ctl', 'shutdown'], env=base)


if __name__ == '__main__':
    main()
