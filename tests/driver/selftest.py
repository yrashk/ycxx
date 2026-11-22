"""The driver's self-test (the policy stage runs it): python3 -m unittest tests.driver.selftest

The fixtures (tests/driver/fixtures/<suite>/...) are tests that must come out a given way; each
says which in its `// FIXTURE: <code>` line. They run with Clang (else GCC) as the compiler
under test, so a driver that passes everything, or fails everything, is caught. The rest checks
the parts that need no compiler: markers, the incremental format, sharding, state and selection.
"""
import json, os, re, runpy, subprocess, sys, tempfile, unittest
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
        # Generous, as in #145: a timeout only guards a hang, and a loaded machine must not turn a
        # fixture's expected outcome into TIMEOUT (own/timeout.run.cpp sets its own).
        cls.ctx = kinds.Ctx(REPO, cut, cls.compilers, 'clang', Path(cls.tmp.name), 120)

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
    def test_draft_initializer_placeholder_fragments(self):
        extractor = runpy.run_path(str(REPO / 'tools' / 'draft-examples'))

        def fragment(source, reports):
            example = extractor['Example']('class.base.init', 'class', 1, 1, 'fixture.tex', 1)
            example.blocks.append(('codeblock', '', source.splitlines()))
            example.build()
            return extractor['is_fragment'](example, reports)

        placeholder = 'struct A {\n  int i = /* some integer expression */ ;\n};\n'
        clang = (1, [(2, 'expected expression')])
        gcc_message = 'expected primary-expression at end of input'
        gcc = (1, [(2, gcc_message)])
        self.assertIn('line 2: expected expression', fragment(placeholder, (clang, gcc)) or '')
        self.assertIsNone(fragment(placeholder.replace('/* some integer expression */', ''), (clang, gcc)))
        self.assertIsNone(fragment(placeholder.replace('some integer expression', 'misplaced annotation'), (clang, gcc)))
        self.assertIsNone(fragment(placeholder.replace('int i', 'return i'), (clang, gcc)))
        self.assertIsNone(fragment('int i = ;\n', ((1, [(1, 'expected expression')]),
                                                  (1, [(1, "expected primary-expression before ';' token")]))))
        self.assertIsNone(fragment(placeholder.replace(';\n', '; // OK\n', 1), (clang, gcc)))
        self.assertIsNone(fragment(placeholder, (clang, (0, []))))
        self.assertIsNone(fragment(placeholder, (clang, (1, [(3, gcc_message)]))))
        self.assertIsNone(fragment(placeholder + 'int main(\n',
                                   (clang, (1, [*gcc[1], (4, "expected ')' at end of input")]))))

    def test_markers(self):
        text = 'a //! error\n//! error@+1:5\nb\nc //! error@1 //! error@-1:2\n'
        self.assertEqual(discover.markers_in(text), [(1, None), (3, 5), (1, None), (3, 2)])
        text = 'a //! warning\n//! error@+1 //! warning@+2:3\n'
        self.assertEqual(discover.markers_in(text), [(3, None)])
        self.assertEqual(discover.markers_in(text, 'warning'), [(1, None), (4, 3)])

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
                # The timeout only guards a hang: this checks a classification, not wall time, and a
                # loaded machine once took 5 s to start the shell script (#145).
                code, output, _ = kinds.execute(test, kinds.Ctx(REPO, cut, {}, 'clang', d / 'work', 120))
                self.assertEqual(code, want, output)

    def test_features_limited_to_hosts(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / 'tests' / 'features').mkdir(parents=True)
            (d / 'tests' / 'features' / 'ycxx.txt').write_text('syntax\nrun darwin-arm64  # one target\n')
            self.assertEqual(toolchain.ycxx_features(d, 'darwin-arm64'), {'syntax', 'run'})
            self.assertEqual(toolchain.ycxx_features(d, 'linux-x86_64'), {'syntax'})
            self.assertRegex(toolchain.HOST, r'^\w+-\w+$')

    def test_os_gates(self):
        """tools/check-os-gates (#659): every one-OS gate names its parity issue or design reason."""
        def check(files):
            with tempfile.TemporaryDirectory() as d:
                for name, text in files.items():
                    (Path(d) / name).parent.mkdir(parents=True, exist_ok=True)
                    (Path(d) / name).write_text(text)
                r = subprocess.run([sys.executable, '-I', str(REPO / 'tools' / 'check-os-gates'), '--root', d],
                                   capture_output=True, text=True)
                return r.returncode, r.stderr
        gated = '// REQUIRES: syntax linux\nint x;\n'
        self.assertEqual(check({'tests/own/a.accept.cpp': gated})[0], 1)
        self.assertEqual(check({'tests/own/a.accept.cpp': gated.replace('linux', 'darwin')})[0], 1)
        self.assertEqual(check({'tests/own/a.accept.cpp': gated + '// PARITY: later\n'})[0], 1)
        self.assertEqual(check({'tests/own/a.accept.cpp': gated + '// PARITY: #12 run it on Darwin\n'})[0], 0)
        self.assertEqual(check({'tests/own/a.accept.cpp': gated + '// PARITY: by design: glibc only\n'})[0], 0)
        self.assertEqual(check({'tests/own/a.accept.cpp': '// REQUIRES: syntax\n'})[0], 0)
        self.assertEqual(check({'tests/features/ycxx.xfail': 'own/a.accept.cpp@linux-x86_64 why\n'})[0], 1)
        self.assertEqual(check({'tests/features/ycxx.xfail': 'own/a.accept.cpp@linux-x86_64 why (#3)\nown/b.accept.cpp no host\n'})[0], 0)
        self.assertEqual(check({'tests/features/ycxx.txt': 'syntax\nrun darwin-arm64\n'})[0], 1)
        self.assertEqual(check({'tests/features/ycxx.txt': '# hosted Linux is #9\nrun darwin-arm64\n'})[0], 0)
        cmake = 'if(APPLE)\n  add_test(NAME t COMMAND t)\nendif()\n'
        self.assertEqual(check({'tests/unit/x/CMakeLists.txt': cmake})[0], 1)
        self.assertEqual(check({'tests/unit/x/CMakeLists.txt': '# PARITY: #5\n' + cmake})[0], 0)
        self.assertEqual(check({'tests/unit/x/CMakeLists.txt': 'if(APPLE)\n  set(X 1)\nendif()\n'})[0], 0)
        for gate in ('if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64)$" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")\n  add_test(NAME t COMMAND t)\nendif()\n',
                     'if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64"\n   AND APPLE)\n  add_test(NAME t COMMAND t)\nendif()\n',
                     'if(APPLE)\n  set(X 1)\nelse()\n  add_test(NAME t COMMAND t)\nendif()\n',
                     'if(WIN32)\n  add_subdirectory(sub)\nendif()\n'):
            self.assertEqual(check({'tests/unit/x/CMakeLists.txt': gate})[0], 1, gate)
        self.assertEqual(check({'tests/incremental/a.incr': '// ACTION: syntax\n--- a.cpp\n// REQUIRES: linux\n'})[0], 0)

    def test_xfail_reasons_keep_issue_numbers(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / 'tests' / 'features').mkdir(parents=True)
            (d / 'tests' / 'features' / 'ycxx.xfail').write_text(
                '# a comment\nown/a.accept.cpp needs x (#12)\nown/b.accept.cpp needs y #34  # trailing comment\n')
            self.assertEqual(toolchain.ycxx_xfails(d), {'own/a.accept.cpp': 'needs x (#12)', 'own/b.accept.cpp': 'needs y #34'})

    def test_xfail_directive_hosts(self):
        t = discover.Test('own/a.accept.cpp', Path('a.accept.cpp'), 'own', 'accept',
                          {'XFAIL': ['gcc@darwin-arm64 only there', 'clang,ycxx@linux-x86_64 two']})
        self.assertEqual(kinds.xfail_reason(t, 'gcc', 'darwin-arm64'), 'only there')
        self.assertIsNone(kinds.xfail_reason(t, 'gcc', 'linux-x86_64'))
        self.assertEqual(kinds.xfail_reason(t, 'clang', 'darwin-arm64'), 'two')
        self.assertEqual(kinds.xfail_reason(t, 'ycxx', 'linux-x86_64'), 'two')
        self.assertIsNone(kinds.xfail_reason(t, 'ycxx', 'darwin-arm64'))

    def test_xfails_limited_to_hosts(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / 'tests' / 'features').mkdir(parents=True)
            (d / 'tests' / 'features' / 'ycxx.xfail').write_text(
                'own/common.accept.cpp common failure\n'
                'own/scoped.accept.cpp@linux-x86_64 Linux failure\n'
                'own/scoped.accept.cpp@darwin-arm64 Darwin failure\n'
                'own/linux.accept.cpp@linux-x86_64 Linux only\n'
            )
            self.assertEqual(toolchain.ycxx_xfails(d, 'darwin-arm64'), {
                'own/common.accept.cpp': 'common failure', 'own/scoped.accept.cpp': 'Darwin failure',
            })
            self.assertEqual(toolchain.ycxx_xfails(d, 'linux-x86_64'), {
                'own/common.accept.cpp': 'common failure', 'own/scoped.accept.cpp': 'Linux failure',
                'own/linux.accept.cpp': 'Linux only',
            })

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

    def test_no_unsupported(self):
        # An UNSUPPORTED test passes a run, but fails it with --no-unsupported, and so does a
        # selection that runs nothing: a CI step that must run its tests cannot go green vacuously.
        # No test runs a compiler here, so ycxx is a stub: the policy stage runs this before ycxx
        # is built (#159).
        inc = 'incremental/smoke/inline-function-body.incr'
        with tempfile.TemporaryDirectory() as d:
            features = Path(d) / 'features.txt'
            features.write_text('syntax\npreprocess\n')  # no "incremental": the test is UNSUPPORTED
            stub = Path(d) / 'ycxx'
            stub.write_text('#!/bin/sh\necho "ycxx stub: never runs a test" >&2\nexit 2\n')
            stub.chmod(0o755)
            env = dict(os.environ, YCXX_FEATURES=str(features), YCXX=str(stub))
            def status(stage, what, *flags):
                return subprocess.run([sys.executable, str(REPO / 'tools' / 'test'), '--cxx', 'ycxx', '--state', d,
                                       '--no-state', '-q', *flags, stage, what], capture_output=True, text=True,
                                      env=env).returncode
            self.assertEqual(status('wider', inc), 0)
            self.assertEqual(status('wider', inc, '--no-unsupported'), 1)
            self.assertEqual(status('core', inc), 0)  # the core stage selects no incremental/ test
            self.assertEqual(status('core', inc, '--no-unsupported'), 1)

    def test_affected(self):
        tests = [discover.Test('own/a.run.cpp', Path(), 'own', 'run'), discover.Test('own/b.run.cpp', Path(), 'own', 'run')]
        self.assertEqual([t.id for t in state.affected(['tests/own/a.run.cpp.stdout', 'README.md'], tests)[0]],
                         ['own/a.run.cpp'])
        self.assertEqual(len(state.affected(['tests/driver/kinds.py'], tests)[0]), 2)
        self.assertEqual(state.affected(['src/parse.cpp'], tests)[0], [])
        self.assertEqual(state.affected(None, tests)[0], [])

    def test_parity_requires_aarch64_load_patterns(self):
        patterns = 'src/target/aarch64/aarch64.patterns'
        inputs = (patterns, 'src/target/aarch64/select.cppm', 'src/target/x86_64/x86_64.cppm',
                  'src/ir/l2.cppm', 'tools/parity/l2-gaps.tsv')
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for name in inputs:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes((REPO / name).read_bytes())
            command = [sys.executable, str(REPO / 'tools/check-parity'), d]
            baseline = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(baseline.returncode, 0, baseline.stderr)

            path = root / patterns
            # Leave the disabled declarations in comments and the selector's load references intact.
            load_rules = r'^([ \t]*pattern[ \t]+[^:\n]+:[ \t]*\([ \t]*load\b[^\n]*)'
            disabled, count = re.subn(load_rules, r'# \1', path.read_text(), flags=re.M)
            self.assertGreater(count, 0)
            path.write_text(disabled)
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn('L2 `load`: the x86_64 back end handles it and aarch64 does not', result.stderr)


if __name__ == '__main__':
    unittest.main()
