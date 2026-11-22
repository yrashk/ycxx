"""Shared mixed-object witnesses for cleanup-free scalar aggregate initialization."""

from pathlib import Path


fixtures = Path(__file__).with_suffix('').resolve()


def check_scalar_aggregate_unwind(compile, run, check_object, host=None):
    witnesses = (
        ('named', ('_Z8selectedv', '_Z10overriddenv'), 'eox'),
        ('temporary', ('_Z10via_directv', '_Z22via_explicit_temporaryv', '_Z21via_literal_temporaryv'), 'nldt'),
    )
    for name, functions, modes in witnesses:
        obj = f'{name}.o'
        compile('-c', fixtures / f'{name}.cpp', '-o', obj)
        check_object(obj, functions)
        if host:
            run(host, '-std=c++20', '-pedantic-errors', '-O0', '-fexceptions', '-fno-lto',
                fixtures / f'{name}_peer.cpp', obj, '-o', 'scalar_aggregate_hosted')
            for mode in modes:
                run('./scalar_aggregate_hosted', mode)
