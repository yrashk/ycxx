"""Check relocated installed-prefix resource resolution for cold and resident ycxx (#1510)."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'daemon'))
from runtime_dir import runtime_root


def run(cmd, cwd, env):
    proc = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, timeout=40)
    return proc.returncode, proc.stdout, proc.stderr


def main():
    ycxx_bin = Path(sys.argv[1]).resolve()
    repo_resources = Path(sys.argv[2]).resolve()
    install_bindir = sys.argv[3]
    install_datadir = sys.argv[4]

    if os.path.isabs(install_bindir) or os.path.isabs(install_datadir):
        return 0

    with (
        tempfile.TemporaryDirectory(prefix='ycxx-relocated-prefix-') as tmp,
        tempfile.TemporaryDirectory(prefix='yr', dir=runtime_root('yr')) as runtime_tmp,
    ):
        root = Path(tmp).resolve()
        prefix = root / 'moved-sdk'
        bindir = prefix / install_bindir
        data_dir = prefix / install_datadir / 'ycxx'
        resources_dir = data_dir / 'resources'
        target_inc = data_dir / 'target' / 'include'
        bindir.mkdir(parents=True)
        shutil.copytree(repo_resources, resources_dir)
        target_inc.mkdir(parents=True)

        # Copy the compiler binary into the relocated prefix and append markers to the copied
        # headers so passing proves resolution came from the relocated prefix, not the checkout.
        relocated_ycxx = bindir / ycxx_bin.name
        shutil.copy2(ycxx_bin, relocated_ycxx)
        (resources_dir / 'linux/include/stddef.h').write_text(
            (resources_dir / 'linux/include/stddef.h').read_text() + '\n#define YCXX_RELOCATED_STDDEF 1\n'
        )
        (resources_dir / 'common/include/stdarg.h').write_text(
            (resources_dir / 'common/include/stdarg.h').read_text() + '\n#define YCXX_RELOCATED_STDARG 1\n'
        )
        (target_inc / 'ycxx_relocated_target.hpp').write_text('#define YCXX_RELOCATED_TARGET 1\n')
        (target_inc / 'stddef.h').write_text('#define YCXX_RELOCATED_STDDEF_WRAPPER 1\n#include_next <stddef.h>\n')

        # Also test invocation through a symlink outside the prefix.
        symlink_dir = root / 'outside-bin'
        symlink_dir.mkdir()
        symlink_ycxx = symlink_dir / ycxx_bin.name
        symlink_ycxx.symlink_to(relocated_ycxx)

        scope = root / 'work'
        scope.mkdir()
        (scope / 'probe.cpp').write_text('''\
#include <stddef.h>
#include <stdarg.h>
#ifndef NO_TARGET_HEADER
#include <ycxx_relocated_target.hpp>
static_assert(YCXX_RELOCATED_TARGET == 1);
static_assert(YCXX_RELOCATED_STDDEF_WRAPPER == 1);
#endif
static_assert(YCXX_RELOCATED_STDDEF == 1);
static_assert(YCXX_RELOCATED_STDARG == 1);
static_assert(sizeof(size_t) == sizeof(void*));
''')

        runtime_dir = Path(runtime_tmp)
        os.chmod(runtime_dir, 0o700)
        base_env = {
            k: v for k, v in os.environ.items()
            if k not in ('YCXX_DAEMON', 'YCXX_NO_DAEMON', 'YCXX_ROOT', 'YCXX_NO_WATCH', 'YCXX_DAEMON_IDLE_EXIT')
        }
        base_env.update(
            LC_ALL='C',
            YCXX_THREADS='2',
            YCXX_ROOT=str(scope),
            YCXX_RUNTIME_DIR=str(runtime_dir),
            YCXX_DAEMON_IDLE_EXIT='120',
        )
        cold_env = base_env | {'YCXX_NO_DAEMON': '1'}
        daemon_env = base_env | {'YCXX_DAEMON': 'require'}

        want_linux = (resources_dir / 'linux/include').resolve().as_posix().encode() + b'\n'
        want_darwin = (resources_dir / 'common/include').resolve().as_posix().encode() + b'\n'

        for exe in (relocated_ycxx, symlink_ycxx):
            res_linux = run([str(exe), 'cxx', '--target=x86_64-pc-linux-gnu', '-print-file-name=include'], scope, cold_env)
            assert res_linux == (0, want_linux, b''), (exe, res_linux, want_linux)
            res_darwin = run([str(exe), 'cxx', '--target=arm64-apple-darwin', '-print-file-name=include'], scope, cold_env)
            assert res_darwin == (0, want_darwin, b''), (exe, res_darwin, want_darwin)

        # Bare PATH invocation must not resolve a decoy file/prefix in the current working directory.
        decoy_prefix = root / 'decoy-sdk'
        decoy_bin = decoy_prefix / install_bindir
        decoy_res = decoy_prefix / install_datadir / 'ycxx' / 'resources'
        decoy_bin.mkdir(parents=True)
        (decoy_bin / ycxx_bin.name).write_text('decoy\n')
        (decoy_res / 'linux/include').mkdir(parents=True)
        (decoy_res / 'common/include').mkdir(parents=True)
        (decoy_res / 'linux/include/stddef.h').write_text('')
        (decoy_res / 'common/include/stdarg.h').write_text('')
        path_env = cold_env | {'PATH': f'{bindir}{os.pathsep}{cold_env.get("PATH", "")}'}
        res_path = run([ycxx_bin.name, 'cxx', '--target=x86_64-pc-linux-gnu', '-print-file-name=include'],
                       decoy_bin, path_env)
        assert res_path == (0, want_linux, b''), (res_path, want_linux)

        try:
            for extra in ([], ['-nostdinc++', '-DNO_TARGET_HEADER']):
                cmd = [str(relocated_ycxx), 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26',
                       *extra, '-fsyntax-only', 'probe.cpp']
                cold = run(cmd, scope, cold_env)
                warm = run(cmd, scope, daemon_env)
                assert cold == warm == (0, b'', b''), (extra, cold, warm)

            # Confirm -nostdinc++ omits target/include even in the relocated prefix.
            cmd_missing = [str(relocated_ycxx), 'cxx', '--target=x86_64-pc-linux-gnu', '-std=c++26',
                           '-nostdinc++', '-fsyntax-only', 'probe.cpp']
            cold_miss = run(cmd_missing, scope, cold_env)
            warm_miss = run(cmd_missing, scope, daemon_env)
            assert cold_miss == warm_miss and warm_miss[0] == 1 and b'file not found' in warm_miss[2], (
                cold_miss, warm_miss)
        finally:
            stop = run([str(relocated_ycxx), 'ctl', 'shutdown'], scope, base_env)
            assert stop[0] == 0, stop

    return 0


if __name__ == '__main__':
    sys.exit(main())
