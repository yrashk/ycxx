"""The driver's self-test (the policy stage runs it): python3 -m unittest tests.driver.selftest

The fixtures (tests/driver/fixtures/<suite>/...) are tests that must come out a given way; each
says which in its `// FIXTURE: <code>` line. They run with Clang (else GCC) as the compiler
under test, so a driver that passes everything, or fails everything, is caught. The rest checks
the parts that need no compiler: markers, the incremental format, sharding, state and selection.
"""
import json, re, subprocess, sys, tempfile, unittest
from pathlib import Path

from . import discover, incremental, kinds, state, toolchain

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
FIXTURES = HERE / 'fixtures'


class Fixtures(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compilers = toolchain.discover(REPO)
        cut = next((c for n, c in cls.compilers.items() if n in toolchain.ORACLES and c.available), None)
        if cut is None:
            raise unittest.SkipTest('neither Clang 23 nor GCC 16 found')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.ctx = kinds.Ctx(REPO, cut, cls.compilers, 'clang', Path(cls.tmp.name), 20)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_fixtures(self):
        tests = discover.discover(FIXTURES)
        self.assertGreater(len(tests), 10)
        for t in tests:
            want = re.search(r'FIXTURE: (\w+)', t.path.read_text())[1]
            needs_both = t.kind == 'diff' or t.one('ORACLE') == 'gcc'
            if needs_both and not all(self.compilers[o].available for o in toolchain.ORACLES):
                continue
            with self.subTest(t.id):
                code, output, _ = kinds.execute(t, self.ctx)
                self.assertEqual(code, want, output)


class Parts(unittest.TestCase):
    def test_markers(self):
        text = 'a //! error\n//! error@+1:5\nb\nc //! error@1 //! error@-1:2\n'
        self.assertEqual(discover.markers_in(text), [(1, None), (3, 5), (1, None), (3, 2)])

    def test_incremental_format(self):
        states = incremental.parse('// h\n--- a.cpp\nx\n--- b.h\ny\n=== edit\n--- b.h\nz\n=== edit\n--- delete b.h\n')
        self.assertEqual(states, [{'a.cpp': 'x\n', 'b.h': 'y\n'}, {'a.cpp': 'x\n', 'b.h': 'z\n'}, {'a.cpp': 'x\n'}])
        for bad in ('--- delete a.h\n', '--- ../x\n', '--- a\n=== frob\n', '=== edit\n',
                    '--- a\nx\n=== edit\n--- delete b\n'):
            with self.assertRaises(ValueError, msg=bad):
                incremental.parse(bad)

    def test_normalize_pp(self):
        self.assertEqual(kinds.normalize_pp('# 1 "a.cpp"\n\nint   x =  1 ;\n#line 3\n#pragma once\n'),
                         ['int x = 1 ;', '#pragma once'])

    def test_shards_partition(self):
        tests = [discover.Test(f't{i}', Path(), 'own', 'run') for i in range(23)]
        secs = lambda tid: 1 + int(tid[1:]) % 5
        parts = [state.shard(tests, secs, i, 4) for i in range(1, 5)]
        ids = [t.id for p, _ in parts for t in p]
        self.assertEqual(sorted(ids), sorted(t.id for t in tests))  # every test once
        loads = [load for _, load in parts]
        self.assertLessEqual(max(loads) - min(loads), 5)  # balanced within the longest test
        self.assertEqual(parts, [state.shard(list(reversed(tests)), secs, i, 4) for i in range(1, 5)])

    def test_state_and_merge(self):
        with tempfile.TemporaryDirectory() as d:
            s = state.State(d, 'main', 'clang')
            s.update({'a': ('FAIL', 1.0), 'b': ('PASS', 2.0)}, 'c1')
            s = state.State(d, 'feature/x', 'clang')
            self.assertTrue(s.inherited)
            self.assertEqual(s.failing, {'a'})  # a new branch starts from main's failures
            self.assertEqual(s.secs('b'), 2.0)
            s.update({'a': ('PASS', 1.5), 'c': ('TIMEOUT', 9.0)}, 'c2')
            s = state.State(d, 'feature/x', 'clang')
            self.assertFalse(s.inherited)
            self.assertEqual(s.failing, {'c'})
            # Two shards restored from the same state; each ran its own tests.
            with tempfile.TemporaryDirectory() as s1, tempfile.TemporaryDirectory() as s2:
                state.State(s1, 'main', 'clang').update({'a': ('PASS', 1.0)}, 'c3')
                state.State(s2, 'main', 'clang').update({'d': ('FAIL', 3.0)}, 'c3')
                state.merge(d, [s1, s2])
            m = state.State(d, 'main', 'clang')
            self.assertEqual(m.failing, {'d'})
            self.assertEqual(m.secs('d'), 3.0)

    def test_crash_is_not_a_rejection(self):
        from .transcript import Run
        ctx = kinds.Ctx(REPO, None, {}, 'clang', Path(), 1)
        f = str(REPO / 'x.cpp')
        r = Run(-11, '', f'{f}:1:1: error: boom\n', 0.0)
        problems, _ = kinds.verdict_problems(ctx, r, {(f, 1, None)}, REPO)
        self.assertTrue(any('crashed' in p for p in problems))

    def test_xfail_list_never_covers_a_crash(self):
        # tests/features/ycxx.xfail turns a failure into XFAIL, but a crash stays a FAIL.
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / 'own').mkdir()
            (d / 'own' / 'a.accept.cpp').write_text('int x;\n')
            test = discover.discover(d)[0]
            for status, want in ((1, 'XFAIL'), (134, 'FAIL')):
                exe = d / f'cxx{status}'
                exe.write_text(f'#!/bin/sh\necho "$3:1:1: error: no" >&2\nexit {status}\n')
                exe.chmod(0o755)
                cut = toolchain.Compiler('ycxx', str(exe), {'syntax'}, {test.id: 'expected'})
                code, output, _ = kinds.execute(test, kinds.Ctx(REPO, cut, {}, 'clang', d / 'work', 5))
                self.assertEqual(code, want, output)

    def test_unsupported_keeps_a_failure(self):
        with tempfile.TemporaryDirectory() as d:
            state.State(d, 'b', 'clang').update({'a': ('FAIL', 1.0)}, 'c1')
            state.State(d, 'b', 'clang').update({'a': ('UNSUPPORTED', 0.0)}, 'c2')
            self.assertEqual(state.State(d, 'b', 'clang').failing, {'a'})

    def test_shards_partition_the_whole_plan(self):
        # With a failing test in the failed stage, the shards of failed, core and wider together
        # list every test exactly once.
        with tempfile.TemporaryDirectory() as d:
            state.State(d, 'b', 'clang').update({'own/smoke/exit-status.run.cpp': ('FAIL', 1.0)}, 'c1')
            Path(d, 'last-run.json').unlink()
            def listed(*extra):
                p = subprocess.run([sys.executable, str(REPO / 'tools' / 'test'), '--list', '--cxx', 'clang',
                                    '--state', d, '--branch', 'b', 'failed', 'core', 'wider', *extra],
                                   capture_output=True, text=True, check=True)
                return [l.split('\t')[0] for l in p.stdout.splitlines() if l and not l.startswith('#')]
            whole = listed()
            parts = [t for i in (1, 2, 3) for t in listed('--shard', f'{i}/3')]
            self.assertEqual(sorted(parts), sorted(whole))
            self.assertEqual(len(parts), len(set(parts)))

    def test_affected(self):
        tests = [discover.Test('own/a.run.cpp', Path(), 'own', 'run'), discover.Test('own/b.run.cpp', Path(), 'own', 'run')]
        self.assertEqual([t.id for t in state.affected(['tests/own/a.run.cpp.stdout', 'README.md'], tests)[0]],
                         ['own/a.run.cpp'])
        self.assertEqual(len(state.affected(['tests/driver/kinds.py'], tests)[0]), 2)
        self.assertEqual(state.affected(['src/parse.cpp'], tests)[0], [])
        self.assertEqual(state.affected(None, tests)[0], [])


if __name__ == '__main__':
    unittest.main()
