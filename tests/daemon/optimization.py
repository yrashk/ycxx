"""Compile the same unit in both optimization modes and verify native results."""

import json
import subprocess
import sys

from process import Env
from system_includes import LINUX, NATIVE_LINUX


SOURCE = '''\
using Word = unsigned long long;
Word high(Word x) { return (x / 9223372036854775808ULL) ^ (x % 9223372036854775808ULL); }
unsigned narrow(unsigned x) { return (x / 2147483648u) ^ (x % 2147483648u); }
unsigned one(unsigned x) { return (x / 1u) ^ (x % 1u); }
long long signed_control(long long x) { return (x / 16LL) ^ (x % 16LL); }
Word non_power(Word x) { return (x / 7ULL) ^ (x % 7ULL); }
Word variable(Word x, Word d) { return (x / d) ^ (x % d); }

int main() {
  Word x = 0;
  for (int i = 0; i < 10000; ++i) {
    if (high(x) != variable(x, 9223372036854775808ULL)) return 1;
    if (narrow(static_cast<unsigned>(x)) != variable(static_cast<unsigned>(x), 2147483648ULL)) return 2;
    if (one(static_cast<unsigned>(x)) != static_cast<unsigned>(x)) return 3;
    if (non_power(x) != variable(x, 7ULL) || signed_control(-79) != 13) return 4;
    x = x * 6364136223846793005ULL + 1442695040888963407ULL;
  }
  return 0;
}
'''

FUSION_SOURCE = '''\
using Word = unsigned long long;
Word quotient(Word x, Word d) { return x / d; }
Word remainder(Word x, Word d) { return x % d; }
Word wide_pair(Word x, Word d) { return ((x / d) << 2) ^ (x % d); }
unsigned narrow_pair(unsigned x, unsigned d) { return (x % d) ^ ((x / d) << 3); }
Word overlapping(Word x, Word d) { return (x % d) + (x / d) * 17 + (x / d) * 3; }
Word different(Word x, Word y, Word d) { return (x / d) ^ (y % d); }
Word forced_loads(volatile Word* x, Word d) { return (*x / d) ^ (*x % d); }
Word changed(Word* x, Word d) {
  Word a = *x / d;
  *x += 1;
  return a ^ (*x % d);
}

int main() {
  Word x = 0;
  for (int i = 0; i < 10000; ++i) {
    Word d = (x >> 41) + 1;
    if (wide_pair(x, d) != ((quotient(x, d) << 2) ^ remainder(x, d))) return 1;
    unsigned y = static_cast<unsigned>(x);
    unsigned e = static_cast<unsigned>(d);
    if (narrow_pair(y, e) != (static_cast<unsigned>(remainder(y, e)) ^
                            (static_cast<unsigned>(quotient(y, e)) << 3))) return 2;
    if (overlapping(x, d) != remainder(x, d) + quotient(x, d) * 20) return 3;
    x = x * 6364136223846793005ULL + 1442695040888963407ULL;
  }
  return 0;
}
'''


def main(ycxx):
    world = Env(ycxx)
    scope = world.scope
    optimizer = '-fycxx-optimize=heuristic'

    def compile(env, *args):
        status, _, stderr = world.cxx([LINUX, *args], scope, env, timeout=30)
        assert status == 0, (args, stderr.decode(errors='replace'))
        return stderr

    def object_bytes(env, *args):
        compile(env, *args, '-c', 'input.cpp', '-o', 'input.o')
        return (scope / 'input.o').read_bytes()

    try:
        (scope / 'input.cpp').write_text(SOURCE)
        baseline = object_bytes(world.daemon, '-O0')
        assert object_bytes(world.daemon) == baseline
        trace = compile(world.daemon, optimizer, '--trace-engine=json', '-c', 'input.cpp', '-o', 'input.o')
        optimized = (scope / 'input.o').read_bytes()
        assert baseline != optimized
        assert object_bytes(world.daemon, optimizer, '-O0') == baseline
        events = [json.loads(line) for line in trace.splitlines()]
        models = [row['value'] for row in events if row.get('kind') == 'DivisionModel' and row['outcome'] == 'executed']
        assert models.count('model=unsigned_power_of_two_division candidates=2') == 3, models
        assert models.count('model=unsigned_power_of_two_division candidates=0') == 4, models
        decisions = [row['value'].splitlines()[0] for row in events
                     if row.get('kind') == 'Optimized' and row['outcome'] == 'executed']
        assert decisions.count('path=heuristic candidates=2 applied=2 validation=passed') == 3, decisions
        assert decisions.count('path=baseline fallback=no_candidates') == 4, decisions
        for workers in ('1', '4'):
            assert object_bytes(dict(world.cold, YCXX_THREADS=workers), optimizer) == optimized

        # PARITY: by design: the ELF object comparison runs everywhere, execution needs Linux x86-64.
        if NATIVE_LINUX:
            images = []
            for mode in ('-O0', optimizer):
                compile(world.daemon, mode, '-nostdlib', 'input.cpp', '-o', 'program')
                subprocess.run([scope / 'program'], cwd=scope, check=True, timeout=30)
                images.append((scope / 'program').read_bytes())
            assert images[0] != images[1]

        (scope / 'fusion.cpp').write_text(FUSION_SOURCE)
        solver = '-fycxx-optimize=solver'
        compile(world.daemon, optimizer, '-c', 'fusion.cpp', '-o', 'fusion.o')
        comparison = (scope / 'fusion.o').read_bytes()
        trace = compile(world.daemon, solver, '--trace-engine=json', '-c', 'fusion.cpp', '-o', 'fusion.o')
        solved = (scope / 'fusion.o').read_bytes()
        assert solved != comparison
        events = [json.loads(line) for line in trace.splitlines()]
        solves = [row for row in events if row.get('kind') == 'DivisionFusionSolve' and row['outcome'] == 'executed']
        assert len(solves) == 3, solves
        assert all(row['worker'] == row['end_worker'] and 'backend=exact_prefix budget=4096' in row['value'] and
                   'outcome=optimal pairs=1' in row['value'] for row in solves), solves
        validations = [row for row in events if row.get('kind') == 'DivisionFusionValidate' and
                       row['outcome'] == 'executed']
        assert len(validations) == 3 and all('validation=passed' in row['value'] for row in validations), validations
        decisions = [row['value'] for row in events if row.get('kind') == 'Optimized' and
                     row['outcome'] == 'executed' and row['value'].startswith('path=solver ')]
        assert len(decisions) == 3 and all('validation=passed' in row for row in decisions), decisions
        for workers in ('1', '4'):
            cold = dict(world.cold, YCXX_THREADS=workers)
            compile(cold, solver, '-c', 'fusion.cpp', '-o', 'fusion.o')
            assert (scope / 'fusion.o').read_bytes() == solved
        if NATIVE_LINUX:
            for mode in (optimizer, solver):
                compile(world.daemon, mode, '-nostdlib', 'fusion.cpp', '-o', 'program')
                subprocess.run([scope / 'program'], cwd=scope, check=True, timeout=30)
    finally:
        world.close()


if __name__ == '__main__':
    main(sys.argv[1])
