# Testing ycxx

One driver, `tools/test`, runs everything (DECISIONS §5.5). It is modelled on libycxx's
(`deps/libycxx/docs/TESTING.md`) and reuses its report page and transcript format. It needs
Python 3.10 or later and nothing else.

## Running

```sh
tools/test --quick                # inner loop: policy build affected-unit failed affected (stops fast)
tools/test --fast                 # before pushing: policy build unit failed affected core (skips slow draft/ XFAILs)
tools/test                        # every stage, without stopping between them
tools/test --tier pr|main|nightly # a CI tier (DECISIONS §9.3), stopping at the first failing stage
tools/test own/smoke              # path filter alone runs test stages only (never triggers build)
tools/test core own/smoke         # stages and path filters, in any order
tools/test --cxx clang own/smoke  # check tests against an oracle (skips build/unit/daemon automatically)
tools/test --only-failed          # just what failed last time
tools/test --skip-xfail core      # skip tests already marked XFAIL for the compiler under test
tools/test --max-failures 5 core  # abort a stage after 5 failures
tools/test --daemon core          # run ycxx tests through a private ephemeral daemon
tools/test --list --shard 2/4     # what shard 2 of 4 would run, with recorded durations
tools/test --help                 # every option
```

The driver prints each command, then live progress (counts, ETA, the longest-running test).
Every test is listed as it finishes, passing ones too, with the steps it ran (`build exit 0
0.13s; run exit 3 0.11s`); a failure shows its whole transcript (the first 10;
`YCXX_FAIL_DETAILS=N`). `-v` prints every transcript, `-q` lists only what did not pass.
Ctrl-C stops gracefully: the stage is cut short, the rest are listed as not run, the report is
written, exit status 130.

Every run writes `build/test-logs/report.html`: every test with its result, steps and full
transcript (the exact commands, exit statuses and output, so a passing test shows how it ran and
a failing one can be rerun by hand), with filters, search and copy buttons that put
self-contained Markdown on the clipboard. The page embeds the whole report as Markdown, also
written as `report.md`; `report.tsv` has one line per test and `report.json` everything, for
tools. Under GitHub Actions the driver also writes the counts and failures to the job summary
and an `::error` annotation per failure.

**Exit status:** 0 all passed (XFAIL and UNSUPPORTED included); 1 a test or stage failed; 2 usage
or configuration error; 3 internal driver error; 130 interrupted.

### Compilers

`--cxx` picks the compiler under test: `ycxx` (default; `$YCXX`, else `build/bin/ycxx`, else
`build/ycxx`), `clang` or `gcc`. The oracles are Clang 23 and GCC 16 (`$YCXX_CLANGXX`,
`$YCXX_GXX`, else libycxx's `toolchains.env`, else `clang++-23` and `g++-16`), always run with
`-std=c++26 -pedantic-errors`. A missing oracle makes the tests that need it UNSUPPORTED;
`--require-oracles` makes it an error (CI uses it). `--no-unsupported` fails the run on any
UNSUPPORTED test, or when the selection runs no test (CI's α slice step uses it).

What ycxx can do is listed in [`tests/features/ycxx.txt`](../tests/features/ycxx.txt): `syntax`,
`preprocess`, `run`, `incremental`. A test that needs a feature ycxx lacks is UNSUPPORTED with
that reason, and the summary counts the reasons. Hosts after a feature limit it to them (`run
darwin-arm64`). When ycxx learns to do something, add the line:
the tests start counting. [`tests/features/ycxx.xfail`](../tests/features/ycxx.xfail) lists the
tests ycxx is expected to fail with the features it has (`<test id> <reason>` per line), outside
the test files because most are generated. A row can use `<test id>@<host> <reason>` to apply on
one host, with the same host names as `ycxx.txt`. An XPASS fails the run, so remove the row when
ycxx passes the test on the specified host. A crash is never expected.

## Stages and fail-fast (DECISIONS §9)

| stage | runs |
|---|---|
| policy | the driver's self-test (`tests/driver/selftest.py`), then `tools/check-policy` if it exists |
| build | `tools/build -j N` (builds with `Dev` `-O1 -g1` by default, plus `-L smoke` CTests: bootstrap and embedding) |
| affected-unit | CTests whose `area:<dir>` label matches changed source files since `--base` |
| unit | CTest unit suite (`ctest -L unit`) |
| failed | tests that failed in the last run of this branch (of `main` for a new branch) |
| affected | tests affected by the change since `--base` (default: merge-base with `origin/main`) |
| core | the core oracles: `tests/draft`, `tests/own` |
| daemon | CTest daemon suite (`ctest -L daemon`: 9 parallel compatibility groups, compare, project_build) |
| wider | `tests/diff`, `tests/incremental` |
| rest | every other suite |

A test runs in the first stage that selects it; within a stage, longest first. The tiers:
`--quick` is `policy`, `build`, `affected-unit`, `failed`, `affected` (`--max-failures 5`, `--skip-xfail`);
`--fast` is `policy`, `build`, `unit`, `failed`, `affected`, `core` (`--max-failures 10`, skipping
known `draft/` XFAILs unless `ycxx.xfail` or `tests/draft/` changed); `pr` adds `daemon` and `wider`;
`main` and `nightly` add `rest`, and nightly also runs tests marked `// TIER: nightly`.

CTests are labeled for direct selective runs via `ctest --test-dir build -L <label>`: `smoke`,
`bootstrap`, `embedding`, `unit`, `area:<subdir>` (e.g. `area:pp`, `area:sema`, `area:pipeline`),
`daemon`, `compat`, `project`, and `heavy`.

**State** (`build/test-state/`, `--state`): `durations.json` (each test's last duration, per
compiler) and `failures/<branch>.json` (the tests failing on that branch). A run updates only the
tests it ran, so a test stays failing until it passes (UNSUPPORTED leaves it as it was). The branch comes from `--branch`, else
`$GITHUB_HEAD_REF`/`$GITHUB_REF_NAME`, else git.

**Sharding** (`--shard I/N`): longest-processing-time-first over the recorded durations, balanced
across all stages, deterministic. All shards must start from the same state; afterwards
`tools/test --merge-state DIR...` merges what each shard learned (its `last-run.json`) into one.

**Affected tests** (`state.affected()`, `state.affected_ctest_labels()`): a changed test file
selects itself; a change to the driver or the features file selects everything; documentation
selects nothing; changes under `src/<area>/` map via coarse area tables (`AREA_TESTS`,
`AREA_CTEST_LABELS`) to the directly relevant `tests/own/` suites and CTest `area:<dir>` labels.
Later: ycxx's own query engine (§9.2.3).

## Writing tests

For a behavior in [the compatibility table](compatibility.toml), exercise each affected
`-fcompat` mode and keep the draft's own expectation in the spec fixture. Use GCC and upstream
Clang as black-box oracles and record the observed versions, target, and evidence for each
mode's behavior. `ctest --test-dir build -R daemon.compatibility` exercises the profiles through
the compiler and editor.

**Unit tests** of ycxx's own components live in `tests/unit/<area>/`, one executable per file,
registered with `ycxxc_unit_test(<name> <source> <libraries>...)` in the area's `CMakeLists.txt`
and run by ctest, so `tools/build` (the driver's build stage, and CI) runs them. They import
`ycxx.unit`: `check(cond, what)` reports each failure with its location, `result(suite)` is the
exit status.
Compile-time properties are `static_assert`s in the same file. The rest of this section is about
the language tests the driver itself runs.

A test's identity is its path under `tests/` (`own/smoke/ok.accept.cpp`): state, sharding and
reports key on it. Its kind comes from its suffix:

| suffix | kind | passes when |
|---|---|---|
| `.accept.cpp` | accept | `-fsyntax-only` succeeds with no error |
| `.reject.cpp` | reject | it fails with errors exactly at the `//!` markers' locations (never the wording) |
| `.pp.cpp` | preprocess | `-E` equals the oracle's after normalization |
| `.run.cpp` | run | it builds and runs with the expected exit status and stdout |
| `.diff.cpp` | diff | the compiler under test agrees with Clang and GCC, which agree with each other |
| `.incr` | incremental | each state's incremental result equals a cold run of it |

**Error markers:** `//! error` expects an error on its own line, `//! error@+1` / `@-2` on a
relative line, `@12` on line 12; `:5` adds a column (`//! error@+1:5`). Every expected location
must get an error and every error must be expected. Markers in a header the test includes with
`#include "..."`, or with `#include <...>` from an `-I` or `-isystem` directory of its `FLAGS`,
expect errors in that header. Locations are parsed from
`file:line:col: error:` lines. A crash (a signal, or an exit status above 1) is never a rejection. Leave the column out where Clang and GCC disagree on it.

A compiler name in brackets limits an error expectation to that compiler under test. For example,
`//! error[gcc]` requires an error on that line from GCC, and `//! error[gcc]@+1:5` requires one on
the next line at column 5. Use `gcc`, `clang` or `ycxx` in brackets.

**Warning markers:** `//! warning`, with the same `@` and `:` forms, expects a warning. An accept
or reject test with any warning marker must get warnings exactly at its warning markers, from
`file:line:col: warning:` lines; a test without one does not look at warnings.

**Directives** (`// KEY: value` lines):

- `REQUIRES: syntax run darwin` — features of the compiler under test, or a platform.
- `PARITY: #N` or `PARITY: by design: <why>` — required with `REQUIRES: darwin` or `linux`: the
  issue that makes the test run on the other OS, or the host fact it is about (that host's C
  library headers, its ABI). `tools/check-os-gates` (in `tools/check-policy`) also requires an
  issue for host-qualified `ycxx.xfail` entries, host-limited features in `ycxx.txt`, and a
  `# PARITY:` comment directly above a platform `if()` in `tests/**/CMakeLists.txt` that adds
  CTests (#659).
- `XFAIL: ycxx,gcc <reason>` (or `*`) — expected to fail with that compiler: reported XFAIL; a
  pass is XPASS, which fails the run, so the mark goes when the cause does.
  `gcc@darwin-arm64` (or `*@linux-x86_64`) limits the expectation to a host (`darwin-arm64`,
  `linux-x86_64`), for an oracle whose answer differs between them.
- `FLAGS: -O2`, `TIMEOUT: 120` (seconds per command), `TIER: nightly`.
- Run tests: `EXIT: 3` (default 0); `STDOUT: line` (repeatable), or a `<test>.stdout` file.
  `PEER: name.cpp` (repeatable): the oracle Clang compiles `name.cpp`, next to the test and not a
  test itself, to an object linked with the test: a mixed-object ABI test (DECISIONS §5.4). The
  peer can call back into the test's functions. `PEER-WITH: apple-clang` compiles the peers with
  Apple Clang (`xcrun clang++`) instead, for tests of `-fclass-call-abi=apple` (§1.6).
- Preprocess tests: `ORACLE: gcc` (default Clang); `PP-COMPARE: exact` keeps whitespace
  (default: line markers and blank lines dropped, whitespace runs collapsed).
- Diff tests: `DIFF: run` (default: verdict, exit status and stdout), `verdict` or `preprocess`.
- Incremental tests: `ACTION: syntax|preprocess|run`, `MAIN: main.cpp`.

**Incremental tests** hold the base state's files and the edits in one file; the format and the
hook for ycxx's incremental mode are in [`tests/driver/incremental.py`](../tests/driver/incremental.py):

```
// ACTION: syntax
--- main.cpp
#include "a.h"
int main() { return f(); }
--- a.h
inline int f() { return 0; }
=== edit
--- a.h
inline int f() { return missing; } //! error
=== edit
--- a.h
inline int f() { return 1; }
--- b.h
int g();
=== edit
--- delete b.h
```

With `ACTION: syntax` each state's cold result must also match the markers in its files; with
`run` or `preprocess` every state must build. Clang and GCC have no
incremental mode: for them the incremental side edits one directory in place and compiles it
cold, which exercises the format and the comparison. ycxx's side is its resident daemon (daemon
design §8.2): a private daemon per test sees each state as file changes. Each request is also
repeated and must execute no engine cell, and an edited state may not execute more cells than a
fresh daemon does for it.

The daemon's own tests are ctest programs in [`tests/daemon`](../tests/daemon): `compare.py`
(daemon versus in-process over `tests/own`, byte for byte) and `process.py` (an edit mid-build,
killed clients and daemons, a restart, concurrent clients), each on a private daemon.

[`tools/incr-fuzz`](../tools/incr-fuzz) generates random token edits over `.incr` tests or C++
files, compares the daemon with a cold compile on every state (`-fsyntax-only`), and writes each
divergence, delta-minimized, as a `.incr` regression test.

**Results:** PASS, FAIL, XFAIL, XPASS, UNSUPPORTED, TIMEOUT, and UNRESOLVED (the test is
malformed, or the oracles disagree: a differential test whose oracles disagree tests nothing
until it is fixed). All but PASS, XFAIL and UNSUPPORTED fail the run.

**The draft suite** (`tests/draft`) is generated from the working draft's examples by
`tools/draft-examples`; it is regenerated, never edited. How, its skips and the oracles'
disagreements: [`tests/draft/README.md`](../tests/draft/README.md).

The driver's own tests are fixtures in `tests/driver/fixtures/`, each saying in a `// FIXTURE:`
line which result it must get.
