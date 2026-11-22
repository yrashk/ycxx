#!/usr/bin/env python3
"""Target C header search (#690), cold and through the same resident daemon."""
import platform
import sys

from process import Env, failures


DARWIN = '--target=arm64-apple-darwin'
LINUX = '--target=x86_64-pc-linux-gnu'
NATIVE_LINUX = sys.platform == 'linux' and platform.machine().lower() in ('x86_64', 'amd64')
GLIBC_SOURCE = '#include <features.h>\nstatic_assert(__GLIBC__ > 0 && __WORDSIZE == 64);\n'


def check(world, cwd, *args, error=None):
    command = ['-fsyntax-only', *args]
    cold = world.cxx(command, cwd, world.cold, timeout=30)
    warm = world.cxx(command, cwd, world.daemon, timeout=30)
    assert warm == cold, (cwd, args, cold, warm)
    if error is None:
        assert warm == (0, b'', b''), (cwd, args, warm)
    else:
        assert warm[0] == 1 and error in warm[2], (cwd, args, warm)


def check_foreign_without_sdk(world):
    (world.scope / 'foreign.cpp').write_text('''\
#if __has_include(<features.h>)
#error host glibc leaked into the Darwin target
#endif
''')
    check(world, world.scope, DARWIN, 'foreign.cpp')


def sources_without_sdk(world):
    scope = world.scope
    if NATIVE_LINUX:
        (scope / 'glibc.cpp').write_text(GLIBC_SOURCE)
        check(world, scope, 'glibc.cpp')
        pid = world.pid()
        check_foreign_without_sdk(world)
        check(world, scope, LINUX, 'glibc.cpp')
        world.same_daemon('native and foreign targets', pid)
    elif sys.platform == 'darwin':
        (scope / 'darwin.cpp').write_text('#include <TargetConditionals.h>\nstatic_assert(TARGET_OS_OSX);\n')
        check(world, scope, DARWIN, 'darwin.cpp')


def sources_with_sdk(world):
    scope = world.scope
    probe = scope / 'provider.cpp'
    probe.write_text('''\
#if defined(__APPLE__) || defined(CHECK_EXPLICIT_ROOT)
#include <ycxx_target_sdk_probe.h>
static_assert(YCXX_TARGET_SDK_PROBE == 42);
#elif __has_include(<ycxx_target_sdk_probe.h>)
#error Darwin SDK leaked into the Linux target
#endif
''')

    def install_sdk(path):
        include = path / 'sdk' / 'usr' / 'include'
        include.mkdir(parents=True)
        (include / 'ycxx_target_sdk_probe.h').write_text('#define YCXX_TARGET_SDK_PROBE 42\n')
        (include / 'features.h').write_text('#error found features.h in the Darwin SDK\n')

    first, second = scope / 'first', scope / 'second'
    install_sdk(first)
    second.mkdir()
    check(world, first, DARWIN, probe)
    pid = world.pid()
    check(world, first, LINUX, probe)
    check(world, second, DARWIN, probe, error=b'ycxx_target_sdk_probe.h')
    check(world, first, DARWIN, probe)
    assert world.number('last_executed') == 0, 'the repeated Darwin request missed its engine cache'
    install_sdk(second)
    check(world, second, DARWIN, probe)
    for flag in ('-I', '-isystem'):
        check(world, first, LINUX, '-DCHECK_EXPLICIT_ROOT', flag, 'sdk/usr/include', probe)
    if NATIVE_LINUX:
        (first / 'glibc.cpp').write_text(GLIBC_SOURCE)
        check(world, first, 'glibc.cpp')
    world.same_daemon('explicit SDK and include directories', pid)


def run(ycxx, test, sdkroot):
    world = Env(ycxx)
    # The final case spells the filesystem root through an absent component unique to this test.
    sdkroot = sdkroot.format(scope=world.scope.name)
    for env in (world.base, world.cold, world.daemon):
        env.update(SDKROOT=sdkroot, YCXX_THREADS='2')
    try:
        test(world)
    finally:
        world.close()


if __name__ == '__main__':
    ycxx = sys.argv[1]
    run(ycxx, sources_without_sdk, '')
    run(ycxx, sources_with_sdk, 'sdk')
    if NATIVE_LINUX:
        run(ycxx, check_foreign_without_sdk, '/{scope}/..')
    sys.exit(bool(failures))
