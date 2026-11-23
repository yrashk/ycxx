"""Suppress implicit C++ headers and retain the other include roots (#997)."""

import json
from pathlib import Path
import re
import sys

from lsp import Editor, expect
from process import Env, failures
from system_includes import LINUX, NATIVE_LINUX


PROBE = '''\
int include_search_probe = 41;
#if !__has_include(<ycxx/config.hpp>)
int cpp_headers() { return missing_cpp_header(); }
#endif
#if !__has_include(<stddef.h>)
int resources() { return missing_resource_header(); }
#endif
#if !__has_include(<features.h>)
int system_headers() { return missing_system_header(); }
#endif
'''
RETAINED = '''\
#include <stddef.h>
#include <features.h>
#ifndef __GLIBC__
#error expected the native GNU C header
#endif
size_t resource_header;
int glibc_header_major = __GLIBC__;
'''
CPP = ('missing_cpp_header',)
ALL = (*CPP, 'missing_resource_header', 'missing_system_header')
NO_CXX = ('-nostdinc++',)
TARGET = (LINUX, '-std=c++26')


def cases(pin_include):
    return (
        ((), ()),
        (NO_CXX, CPP),
        ((*NO_CXX, '-I', pin_include), ()),
        (('-isystem', pin_include, *NO_CXX), ()),
        ((*NO_CXX, '-nostdinc'), ALL),
        (('-nostdinc', *NO_CXX), ALL),
        (NO_CXX, CPP),
        ((), ()),
    )


def command_line(world, scope, modes):
    (scope / 'probe.cpp').write_text(PROBE)
    (scope / 'retained.cpp').write_text(RETAINED)

    def preprocess(source, flags):
        args = [*TARGET, *flags, '-E', source]
        cold = world.cxx(args, scope, world.cold, timeout=30)
        warm = world.cxx(args, scope, world.daemon, timeout=30)
        assert cold == warm and cold[0] == 0 and not cold[2], (args, cold, warm)
        return cold[1]

    snapshots = []
    for flags, missing in modes:
        tokens = preprocess('probe.cpp', flags)
        assert b'include_search_probe' in tokens, tokens
        assert tuple(name for name in ALL if name.encode() in tokens) == missing, (flags, tokens)
        if not snapshots:
            pid = world.pid()
        snapshots.append(tokens)
    assert snapshots[0] == snapshots[-1] and snapshots[1] == snapshots[-2]

    retained = preprocess('retained.cpp', ())
    assert re.search(rb'\bglibc_header_major\s*=\s*[1-9][0-9]*\s*;', retained), retained
    assert preprocess('retained.cpp', NO_CXX) == retained
    return pid


def compile_database(world, scope, modes):
    unit = scope / 'listed.cpp'
    unit.write_text(PROBE)
    database = scope / 'compile_commands.json'
    editor = Editor(world, scope)
    for version, (flags, missing) in enumerate(modes, 1):
        database.write_text(json.dumps([{
            'directory': str(scope), 'file': unit.name,
            'arguments': ['c++', *TARGET, *flags, '-c', unit.name, '-o', 'listed.o'],
        }]))
        if version == 1:
            editor.open(unit, PROBE)
        else:
            editor.change(unit, PROBE, version)
        expected = []
        for name in missing:
            line, spelling = next((i, row) for i, row in enumerate(PROBE.splitlines()) if name in row)
            expected.append((line, spelling.index(name), 1, f"use of undeclared identifier '{name}'"))
        context = f'nostdinc++ compile database version {version}'
        expect(editor.diagnostics(unit, version, context), expected, context)
    expect(editor.finish(), 0, 'nostdinc++ LSP shutdown')


def main():
    # PARITY: by design: witnesses the native Linux x86-64 GNU C header and resource paths.
    if not NATIVE_LINUX:
        return 0
    ycxx, libycxx = (Path(arg).resolve() for arg in sys.argv[1:])
    pin_include = libycxx / 'include'
    assert (pin_include / 'ycxx/config.hpp').is_file(), pin_include
    world = Env(str(ycxx))
    scope = world.scope.resolve()
    for env in (world.base, world.cold, world.daemon):
        env.update(LC_ALL='C', YCXX_THREADS='2')
    try:
        modes = cases(str(pin_include))
        pid = command_line(world, scope, modes)
        compile_database(world, scope, modes)
        world.same_daemon('nostdinc++ command line and compile database', pid)
    finally:
        world.close()
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
