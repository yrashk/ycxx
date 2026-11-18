# Spike: solver libraries built with GCC 16.2, against libstdc++ and against libycxx

Throwaway build spike for #3. It asks which solver libraries build from source with GCC 16.2,
both against libstdc++ and against libycxx as the standard library. The answer decides which
solver back ends come first behind the constraint-model layer. Run on 2026-11-17, macOS 26 arm64
(18 cores), Homebrew GCC 16.2.0, CMake 4.4.4, Ninja.
libycxx was at `866f6784` (`deps/libycxx`), built with GCC into a scratch prefix.

## How it was run

`build.sh` does everything. Sources are shallow clones in a scratch directory `$S`
(`$S/src-<name>/<name>`), never in the repo:

```sh
export S=/some/scratch/dir
./build.sh libycxx                  # libycxx: GCC 16.2, Release, installed to $S/libycxx-gcc
./build.sh cadical std|ycxx         # likewise highs, z3, bitwuzla, gecode, chuffed
```

- **std**: `g++-16`/`gcc-16` and libstdc++.
- **ycxx**: libycxx's installed toolchain file (CMake) or its `ycxx-c++`/`ycxx-cc` wrappers
  (CaDiCaL's own configure, Bitwuzla's meson). Every resulting binary passes `ycxx-check-binary`
  ("libycxx, static").
- **Smoke tests**:
  - CaDiCaL: a 4-clause SAT/UNSAT pair with a conflict limit.
  - HiGHS: a 2-variable MIP with threads=1, a fixed seed and a node limit, then 4 instances from
    HiGHS's `check/instances`.
  - Z3 and Bitwuzla: prove `x*2 == x<<1` for all 32-bit `x` and refute `x*3 == x<<1`.
  - Gecode and Chuffed: a 2-variable FlatZinc model, solved by `fzn-gecode` and `fzn-chuffed`.

Times are wall-clock with `-j4`–`-j8`. Several builds ran at once, so treat them as rough.

## Results

| Solver (rev) | std build | ycxx build | Smoke (std / ycxx) | Licence, mandatory deps | Deterministic limits |
|---|---|---|---|---|---|
| CaDiCaL 3.1.0 | PASS 18 s | PASS 17 s | PASS / PASS | MIT; none | `limit("conflicts"\|"decisions"\|"preprocessing"\|"localsearch", n)`, `seed`; single-threaded |
| HiGHS 1.16.0 | PASS 49 s | PASS 57 s, after a 1-line workaround for a GCC ICE (B2) | PASS / **FAIL**: tiny MIP ok; `afiro`, `egout` segfault in presolve (B1) | MIT; none (zlib, BLAS optional, off) | `threads=1`, `random_seed`, `mip_max_nodes`/`_leaves`/`_stall_nodes`/`_improving_sols`, `simplex_`/`ipm_`/`pdlp_iteration_limit`; `time_limit` is wall-clock |
| Z3 5.4.2 | PASS 499 s | PASS 465 s | PASS / PASS | MIT; none (GMP optional, off) | `rlimit` (deterministic counter) vs `timeout` (wall-clock); `max_conflicts`; seeds |
| Bitwuzla 0.10.0-dev | PASS 42 s | PASS 45 s | PASS / PASS | MIT; **GMP and MPFR mandatory** (LGPL-3.0+); SymFPU (BSD-3 or GPL-3); CaDiCaL (MIT) | Only `time-limit-per` (wall-clock) and `memory-limit`; `seed`, `nthreads`. No deterministic budget |
| Gecode 6.6.0 | PASS 215 s | PASS 209 s | PASS / PASS | MIT; none (MPFR, Qt optional, off) | `-node`, `-fail` (deterministic), `-time` (wall-clock), `-p 1`, `-r` seed. **FlatZinc: yes** (`fzn-gecode`) |
| Chuffed 0.14.0 | PASS 25 s | PASS 39 s, with `CMAKE_DISABLE_FIND_PACKAGE_ZLIB` (B3) | PASS / PASS | MIT; none | Only `-t` (wall-clock); `--rnd-seed` defaults to 0, which means time-seeded, so it must be set. **FlatZinc: yes** (`fzn-chuffed`) |
| Kissat 4.0.4 | not built: C99 only, so libycxx does not apply | — | — | MIT; none | `--conflicts`, `--decisions` |

Revisions: CaDiCaL `6b3d70b`, HiGHS `f3f113d`, Z3 `153b2a3`, Bitwuzla `47db2d3` (its CaDiCaL
3.0.1 subproject was built with libycxx too), Gecode `c994b54`, Chuffed `f698d56`.

Bitwuzla cannot be built without GMP: `src/meson.build` calls `dependency('gmp')` and
`dependency('mpfr')` unconditionally. Both are C libraries, so they link fine with libycxx; the
issue is the licence, not the build.

## Determinism check across standard libraries

- **CaDiCaL**: on pigeonhole 9→8, each build is repeatable. Two runs of the same binary give the
  same conflict count, but the two builds differ: 52 405 conflicts (libstdc++) against 44 220
  (libycxx). The search path depends on unspecified library behaviour, such as `std::sort` order
  among ties. **A conflict budget is reproducible only for one solver binary built with one
  standard library.**
- **Z3**: factoring a 32-bit semiprime gives `rlimit count` 107 174 with both builds. A run with
  `rlimit=50000` returns `unknown` ("max. resource limit exceeded") in both.
- **HiGHS**: on instances that run under both builds (`25fv47`, `bell5`, `flugpl`, `lseu`,
  `adlittle`, `blending`), iteration counts, node counts and objectives match.

## libycxx bugs found (for upstream issues)

- **B1: `std::set`/`std::map` `end()` changes on the first insertion** (yrashk/libycxx#5). In
  `include/ycxx/core/rb_tree.hpp`, the header is set up lazily: `__hdr_` is `nullptr` "before
  the first insertion". So `end()` of a container that never had an element is `iterator(nullptr)`,
  and it compares unequal to `end()` after an `insert`/`emplace`. That violates
  [associative.reqmts.general]: insert and emplace "shall not affect the validity of iterators".
  The repro below prints 1 with libstdc++ and 0 with libycxx:
  ```cpp
  std::set<int> s; auto e = s.end(); s.insert(1); return e == s.end();
  ```
  HiGHS does exactly this. `HPresolve` fills `eqiters` with `equations.end()` while the set is
  still empty, then compares with `!=` and calls `erase` on the iterator. That leads to a SIGSEGV
  in `__ycxx::__detail::__rb_erase`, called from `HPresolve::markRowDeleted`.
- **B2: GCC 16.2 ICE triggered by libycxx's `abs` overload set** (the same ICE as yrashk/libycxx#1; this trigger is added there). The error is
  `internal compiler error: in compare_ics, at cp/call.cc:12857`. Repro:
  ```cpp
  #include <vector>
  double f(const std::vector<double>& a) { return abs(a[0]); }
  ```
  `a.front()` triggers it too. `std::abs(a[0])`, `abs(local)` and libstdc++ all compile. libycxx
  implements `abs` as constrained templates such as
  `template<class _Tp = double> requires … _Tp abs(type_identity_t<_Tp>)`, re-exported by a global
  `using std::abs;`. This is a GCC bug, but libycxx should probably work around it. HiGHS
  workaround: qualify the calls in `HEkkDebug.cpp` (in `build.sh`).
- **B3 (robustness/docs): the C wrapper headers are bypassed by a plain `-I` to the C headers** (yrashk/libycxx#6).
  Chuffed's `find_package(ZLIB)` adds `-I…/MacOSX.sdk/usr/include`. The SDK's `<stdlib.h>` and
  `<string.h>` then win over libycxx's `-isystem` wrappers. libycxx's `<cstdlib>` then fails with
  "declaration of 'int atexit(void (*)()) noexcept' has a different exception specifier", and
  `<cstring>` with "'const char* std::__y1::strchr(const char*, int)' conflicts with a previous
  declaration". libstdc++ tolerates this setup. It should either be made robust or be listed under
  "Common errors" in BUILDING_PROJECTS.
- **Noise.** GCC reports `-Wstringop-overflow` false positives in `vector<int>::resize` →
  `construct_at` (`memory_base.hpp:220`) under `-O3`: 6 in CaDiCaL, 23 in Bitwuzla. Z3 also gets
  645 deprecation warnings for `operator<<(ostream&, unsigned char)`.

## Conclusions

1. **CaDiCaL first (SAT).** It is clean under both libraries, has no dependencies, uses MIT, and
   has deterministic conflict and decision limits.
2. **Z3 first for SMT/translation validation.** It is clean under both libraries, has no
   dependencies, and uses MIT. `rlimit` is deterministic, and it matched exactly across
   standard libraries. The cost is size: about 8 minutes to build the static library at `-j6`.
   Bitwuzla builds and is faster, but it needs LGPL GMP/MPFR and has no deterministic budget.
   It can be a second SMT back end, not the first.
3. **HiGHS (LP/MIP): ready for libstdc++, blocked on libycxx by B1** (a crash on ordinary LPs).
   It needs the B2 workaround or a libycxx fix too. Make it the first MIP back end once B1 is fixed.
4. **CP: Gecode before Chuffed.** Both build cleanly and read FlatZinc. Only Gecode has
   deterministic node and fail limits; Chuffed has wall-clock time only and a time-based default
   seed.
5. **For the constraint-model layer**, budgets must use solver-deterministic units (conflicts,
   `rlimit`, nodes/fails, iterations), never wall-clock. Reproducibility also requires pinning the
   exact solver build, including its standard library, as the CaDiCaL result shows.

Also learned: `if run; then …` in bash disables `set -e` inside `run`. The first version of
`build.sh` reported HiGHS-on-libycxx as PASS while it was segfaulting. It now runs
`(set -e; run)` outside any condition.
