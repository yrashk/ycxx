# E-graph spike (throwaway)

A mechanism spike for [yrashk/ycxx#3](https://github.com/yrashk/ycxx/issues/3) (optimization
architecture). It is not the start of an optimizer and may be deleted once the design is accepted.
It was written from the published papers (egg, POPL 2021; egglog, PLDI 2023; Tate et al.,
POPL 2009; extraction as an ILP), not from any e-graph tool's source. It builds standalone with
GCC 16.2 and plain libstdc++; HiGHS (MIT) is built from source outside the repository:

```bash
make DEPS=/some/scratch/dir deps     # clones HiGHS at a pinned commit and builds it with $(CXX)
make DEPS=/some/scratch/dir check    # ~30 s
make DEPS=/some/scratch/dir san      # ASan + UBSan
make CXX=clang++ HIGHS=/path/to/clang-built-highs tsan   # GCC has no TSan on macOS/arm64
```

## What is in it

- `egraph.hpp` (~390 lines): union-find whose canonical id is always the smaller one, hash-consed
  e-nodes, egg-style deferred congruence closure (parent lists, a worklist, `rebuild()` after each
  apply phase), and a constant-folding analysis whose `modify` adds the constant's e-node. Pure
  integer IR: `const var neg + - * <<`, i64 wrapping, shift amount mod 64. Thirteen rules: `+`/`*`
  commutativity and associativity, `x*2^k -> x<<k`, `x*2c -> (x*c)<<1`, `x*b+x*c -> x*(b+c)`,
  `x+x -> x*2`, `x-x -> 0`, `a-b -> a+(-b)`, `--x -> x`, `x+0`, `x*1`, `x*0`. Saturation runs
  match (read-only, optionally one thread per rule) then apply (sequential, rule order then match
  order) then rebuild, under an iteration limit and an e-node limit checked before every
  application.
- `extract.hpp`: greedy bottom-up extraction (tree cost; ties go to the smaller e-node) and an ILP
  extraction over HiGHS's C API that pays every chosen e-node once (DAG cost).
- `main.cpp`: the commands below. Every extracted term is evaluated against the input on random
  inputs; a mismatch aborts.

Costs: `const var neg` 1, `+ - <<` 2, `*` 4.

## What it shows

Measured on an 18-core Apple M-series machine, GCC 16.2 `-O2`, HiGHS 1.16.0 (`f3f113d`).

### Determinism of saturation (`spike determinism`, `spike threads 8`)

A 120-entry random expression, budget 8 iterations / 30,000 e-nodes (the e-node limit is hit
in iteration 5). Five runs: hash seeds `1, 1, 0xdeadbeef, 12345, 99`, initial bucket counts
`0, 0, 0, 65536, 7`. The fingerprint covers every canonical id, analysis value and e-node, plus
the extracted term and its cost.

| Visit order for matching | e-classes | e-nodes | distinct fingerprints over 5 runs |
| --- | --- | --- | --- |
| class id (the design) | 7,399 in all runs | 29,608 in all runs | **1** |
| hash-table iteration (counter-example) | 7,326 – 7,351 | 29,725 – 29,822 | **4** (only the two seed-1 runs agree) |

What was required: (1) new ids from a counter, so ids are a function of the order of `add`
calls; (2) the smaller id stays canonical on `merge`; (3) matching visits classes in id order
and nodes in sorted order; (4) the rebuild worklist is sorted and deduplicated; (5) the hash
table is only probed with exact keys, never iterated; (6) the e-node budget is checked before
each application, in that fixed order. The counter-example breaks only (3), and the budget then
cuts a different prefix of the matches. The same id-order fingerprints also came out of a Clang
build against libc++, whose hash table differs from libstdc++'s.

Parallel matching, 120-entry input, 8 iterations / 300,000 e-nodes (137,211 classes, 297,672
e-nodes at the end): fingerprint `d3385a1eaa6b7cbe` at 1, 2 and 8 threads. Matching has one
result slot per rule, so the thread count cannot reorder matches. It brings no speedup here:
match is 40 → 26 ms of 311 ms, while rebuild takes 174 ms and apply 66 ms. TSan is clean.

### Greedy vs ILP extraction (`spike demo`, `spike random 50 30 300`)

| Input | e-classes / e-nodes | greedy tree / DAG cost | ILP DAG cost | ILP ms |
| --- | --- | --- | --- | --- |
| `x*3 + x*5` | 7 / 15 | 4 / 4 `(<< x 3)` | 4 | 1.2 |
| `(x+y) - (y+x)` | 7 / 17 | 1 / 1 `0` | 1 | 0.1 |
| `x*3 - x*6` | 8 / 13 | 14 / 13 | **11** `(- (* x 3) (<< (* x 3) 1))` | 0.1 |
| `x*y - (x*y)*6 - (x*y)*12*z` | 38 / 163 | 37 / 33 | **23** | 20 |
| `x*5*y - x*10 - x*20 - x*40` | 28 / 84 | 35 / 32 | **24** | 7 |

`x*3 - x*6` is the shape where greedy loses: per class, `x*6` (tree cost 6) beats
`(x*3)<<1` (tree cost 9), but `x*3` is needed anyway, so the shift form costs 3 more in the DAG,
not 9. Greedy never sees this because tree cost counts a shared child once per use.

On 50 random 30-entry expressions saturated to ≤ 301 e-nodes, greedy's DAG cost is above the
ILP optimum in **40 of 50**, by **8.7 %** in total (2,976 vs 2,738) and **25.7 %** at worst.
Greedy's DAG cost is not even monotone in saturation: on the 50-entry growth input it goes 78
→ 81 → 78 → 73 over iterations 3–6.

ILP cost grows fast with e-graph size (one thread, after pruning):

| e-nodes | columns / rows | solve time | branch-and-bound nodes |
| --- | --- | --- | --- |
| ≤ 301 (50 models) | 163 – 228 / 291 – 428 | mean 220 ms, max 0.78 s | 1 – 96 |
| 600 | 447 / 859 | 0.88 s | 177 |
| 1,000 (3 models) | 625 – 686 / ~1,300 | 3.1 – 14 s | 350 – 6,385 |

Pruning dominated e-nodes (same class, no cheaper, child classes a superset of another
e-node's) is what makes even the small sizes usable: without it the same 300-e-node models take a
mean 2.4 s (first 10 models) instead of 0.22 s, because commutativity's mirror images make the search visit
symmetric twins.

**Cycles.** An e-class can (indirectly) contain itself: `x+0` sits in `x`'s class, `--x` makes a
two-class cycle. A pick that only requires "every chosen e-node's children have a chosen e-node"
can then select a cycle and nothing real. The ILP drops e-nodes whose child is their own class
(321 across the 50 models), computes strongly connected components of the class graph, and for
edges inside an SCC only adds a potential `t_c ∈ [0, |SCC|-1]` with
`t_c ≥ t_d + 1 − |SCC|·(1 − x_n)` (Miller–Tucker–Zemlin); edges between SCCs cannot close a
cycle and get no row. 22 of the 50 models had cycles (333 classes with potentials). With the
rows removed (`NO_ACYCLIC=1`), **17 of 50** optimal picks are cyclic, i.e. not programs. Greedy
needs nothing: with all costs ≥ 1 a child is always strictly cheaper than its parent.

### HiGHS determinism (`spike highs 1`, `spike highs -1`)

One 300-e-node model (163 columns, 291 rows), solved five times per configuration in one
process, then repeated across processes:

| Options | objective | simplex iterations | B&B nodes | solution |
| --- | --- | --- | --- | --- |
| `threads=1, random_seed=0` (×5, ×2 processes) | 70 | 11,967 | 38 | `75bb…` every time |
| `threads=1`, seeds 1 / 2 / 3 | 70 | 21,214 / 9,819 / 14,097 | 96 / 46 / 36 | three other optima |
| `threads=2, seed 0` | 70 | 14,914 | 28 | `75bb…` |
| `threads=4, seed 0` | 70 | 9,981 | 13 | `1f93…` |
| `threads=8` or default, `seed 0` (×5, ×3 processes) | 70 | 9,452 | 9 | `1f93…` every time |

HiGHS is reproducible for a fixed `(threads, random_seed)`, but which of several equal-cost
optima it returns depends on both, and the default thread count depends on the machine. So a
compiler must set `threads` and `random_seed` explicitly (and `mip_rel_gap=0`, `output_flag=false`),
never use `time_limit` (bound work with `mip_max_nodes` if needed; not measured here), and pass
rows and columns in a deterministic order. Making the optimum unique (a tie-break in the
objective) would remove the dependence on solver settings; not tried.

### Growth (`spike growth 100`)

No iteration limit, 2,000,000 e-node cap, random inputs over 4 variables:

| iteration | 50-entry input: e-nodes / ms | 100-entry: e-nodes / e-classes / ms | 200-entry: e-nodes / ms |
| --- | --- | --- | --- |
| 0 | 61 | 120 / 101 | 223 |
| 1 | 124 / 0 | 255 / 125 / 0 | 488 / 0 |
| 2 | 269 / 0 | 553 / 240 / 0 | 1,243 / 1 |
| 3 | 927 / 0 | 2,031 / 798 / 1 | 6,198 / 3 |
| 4 | 5,246 / 3 | 14,074 / 5,528 / 8 | 75,680 / 160 |
| 5 | 48,013 / 81 | 235,733 / 95,938 / 439 | 1,084,589 (cap) / 17,686 |
| 6 | 607,697 / 2,310 | 1,972,273 (cap) / 857,846 / 4,418 | |

Associativity and commutativity alone make growth 5–15× per iteration from iteration 3 on; none
of these inputs saturates. The greedy tree cost of the 100-entry input keeps falling until the
cap (597 → 399), so the budget, not saturation, decides the result.

## What it leaves out

Control flow, memory, side effects, and anything but integer arithmetic; rule scheduling (egg's
backoff scheduler) and incremental matching (egglog's relational e-matching, semi-naive
evaluation); merging analyses beyond constants; proofs or explanations; translation validation;
extraction costs that depend on the target; LP-based or heuristic extraction between greedy and
exact ILP; an ILP warm start from greedy; HiGHS under `mip_max_nodes`. The final memo rebuild in
`rebuild()` is a simplification egg avoids. The rebuild, not matching, dominates time here, so
parallel matching alone is not where the speedup is.
