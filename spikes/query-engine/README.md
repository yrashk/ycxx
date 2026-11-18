# Query-engine spike (throwaway)

A prototype for [yrashk/ycxx#1](https://github.com/yrashk/ycxx/issues/1) and
[`docs/design/query-engine.md`](../../docs/design/query-engine.md). It is not the start of the
real engine and may be deleted once the design is accepted. It builds standalone because ycxx's
build skeleton (#4) does not exist yet:

```bash
make CXX=clang++ check
```

## What is in it

- `engine.hpp` (~500 lines): a red/green query engine. Keys are totally ordered; values are
  compared by equality for early cutoff; each entry stores `changed_at`, `verified_at` and its
  dependencies as an ordered list of *groups* (a group is the keys one parallel fetch asked for).
  Validation walks the groups in order, validating each group's members in parallel, and
  re-executes on the first group with a member that changed. An entry is claimed before it is
  validated or executed, so every query runs at most once per revision (unless it throws: a
failed claim is released and a waiter retries it). Parallel fetches are
  fork-join on a work-stealing pool; a joining worker only runs tasks of its own join.
- `toy.hpp`: a language with `int`/`float`/`bool`, functions, `include`, and C++-like overload
  resolution (exact < promotion < conversion, [over.match.best], ambiguity). Its queries mirror the
  design: per-item projections with positions relative to the item, a per-`(file, name)`
  declaration firewall (`DECLS_NAMED`), per-`(TU, name)` overload sets, and resolution memoized
  per `(TU, name, argument types)`.
- `main.cpp`: the demonstrations below.

## What it shows

| Command | Shows |
| --- | --- |
| `spike demo` | (a) editing `local`'s body re-executes `CHECK`/`LOWER` of `local` only; moving code down re-checks nothing (relative positions). (b) adding `f(float)` to the header re-runs exactly the three resolutions that name `f`, only one changes its winner, and only `use_float` is re-checked; adding `k` flips a *negative* lookup and re-checks only its caller; adding an unrelated declaration re-runs no resolution at all. |
| `spike threads 16 10` | (c) a 2,000-function program built at 1, 2, 4, 8 and 16 threads, cold and then after each of 10 random edits: identical output, identical set of executed queries and cutoffs, identical memo graph (revisions, dependency lists, execution counts) at every step. A cold build alone can only differ in what runs and in dependency order; the incremental steps are where validation and cutoff could depend on scheduling. Near-linear speedup. |
| `spike fuzz 1000 8` | The incremental == cold invariant: random edits (bodies, declarations added and removed, includes toggled, headers deleted and restored, positions shifted, functions added and removed); after every edit the incremental results at 1 and 8 threads equal a cold build, and the two incremental engines executed the same queries. |
| `spike fuzz-bug 300 8` | The same checker with one dependency deliberately left unrecorded (`RESOLVE` reads `OVERLOADS` untracked). It must fail with incremental != cold (exit code 3), and does within a few edits. |

Results on an 18-core Apple M-series machine (Apple Clang 21, `-O2`): demo all checks pass; cold
build 0.77 s at 1 thread, 0.10 s at 8, 0.05 s at 16 (simulated work per body); 1,000 fuzz edits
pass; the injected bug is caught at edit 8. ASan, UBSan and TSan builds run clean.

## What it leaves out (covered by the design document, not the spike)

Point-of-declaration ordering, scopes and ADL; cancellation (inputs only change between builds);
cycle detection beyond the requesting chain (cycles between parallel siblings would deadlock;
the toy language has no cycles); garbage collection; persistence; the preprocessor.
