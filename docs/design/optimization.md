# Optimization architecture: IR, equality saturation, solvers, verification

Design for [yrashk/ycxx#3](https://github.com/yrashk/ycxx/issues/3). Status: **accepted**; §1 is
summarized in DECISIONS §4.8, which cites this document's sections as §O1–§O14.

This document sits on the accepted [query-engine design](query-engine.md): the optimizer is a
set of queries (§A8), with the constraints its Part D lists. It is sized to its decisions
(DECISIONS §8.6): each decision, its reasons and the alternatives rejected. Mechanism questions
are settled by the spikes in `spikes/optimization/` (S1: toy e-graph and extraction, §3.5; S2:
solver builds, §4.2).

| Issue question | Where |
| --- | --- |
| IR shape, levels, effects, interpreter needs, identity, debug info | §2 |
| Equality saturation: level, rules, proofs, extraction, budgets, determinism | §3 |
| Constraint-model layer and solver back ends | §4 |
| Combinatorial code generation | §5 |
| Verification boundary, UB and refinement, evidence | §6 |
| Budget ladder and the optimization cache | §7 |
| Cost models | §8 |
| Performance oracle and the work list | §9 |
| What M4 and M5 must build, and what they may skip | §10 |
| Constraints on #2 and #1 | §11 |
| Rejected alternatives, follow-up issues, open items, provenance | §12–§14 |

## 1. Decisions in brief

1. **One IR, three levels.** L1 *semantic* (front end writes it, interpreter runs it: typed access
   paths, abstract pointers, explicit lifetime), L2 *lowered* (flat bytes, fixed widths, poison:
   where search and SMT live), L3 *machine*. (§2)
2. **Effects are values.** State is an SSA value threaded through effectful operations; control
   effects are terminators. Pure speculatable nodes float, everything else is pinned to its block,
   so a function's dataflow is an acyclic DAG the e-graph can hold. (§2.2)
3. **The interpreter is the semantics.** L1 carries every fact [expr.const] needs as an explicit
   operation or attribute; L2 is never interpreted in constant evaluation. (§2.3)
4. **Equality saturation at L2** over the whole function, block arguments as leaves, with rules
   that are *equivalences* over the poison-aware total algebra, written as data, proven once per
   optimizer version by SMT. Greedy extraction everywhere; exact ILP (HiGHS) on e-graphs small
   enough for it. (§3)
5. **A ycxx-owned flat constraint model** (`ycxx::cm`) exported to FlatZinc; back ends HiGHS,
   CaDiCaL, Z3, Gecode in that order, Chuffed and CP-SAT by export first; all single-threaded
   under deterministic unit budgets. (§4)
6. **Combinatorial code generation improves on a heuristic that always exists**, both reading one
   pattern table and both ending in the same validators. (§5)
7. **Verification boundary.** Proven rules: correct by construction. Solver answers: checked.
   Other code-changing passes: SMT-validated per acyclic region where the level affords it, else
   trusted and listed. Evidence is a value; replay re-checks it. (§6)
8. **The ladder**: 0 naive; 1 cleanups and a small greedy e-graph; 2 inlining, loop passes, ILP
   extraction on small e-graphs, bounded TV on small changed regions; 3 combinatorial codegen and superoptimization; above 3, every budget × 4 per
   level. (§7)
9. **Cost models are measured tables**, one per (target, microarchitecture), a high-durability
   input whose fingerprint is in the `target` key. (§8)
10. **The oracle feeds a daemon-side work list** that orders background roots and never reaches a
    value; verified superoptimizer results become rule candidates for agents. (§9)
11. **M4 and M5 build L1, the interpreter, `lower`, the pattern table, the validators and the §A8
    plumbing, with level 0 and level 1's code generation (linear scan, list scheduling). No
    e-graph, rule, solver or SMT.** (§10)

## 2. The IR

### 2.1 Levels

| | L1 semantic | L2 lowered | L3 machine |
| --- | --- | --- | --- |
| Produced by | `function_ir(F, ctx)` from the analysed body | `lower(ir)` | `optimized(unit, …)` |
| Values | Typed: C++ scalar types, pointers as abstract `(object, path)`, aggregates by access path | `iN`, `fN`, `ptr` (an `i64` with provenance facts), tuples | Register classes, immediates, memory operands |
| Memory | `load/store(state, ptr, type, path)`, `obj.begin/end`, `union.activate`, `alloc(storage, type, init)` | Byte-addressed `load/store(state, ptr, iN, alias tag)`, `alloca(size, align)`, `lifetime.end`; the alias tag is derived from the L1 access type, ignored by equivalence and read by alias analysis | Frame slots, addressing modes |
| Control | Blocks with arguments; `br`, `switch`, `ret`, `call` with normal/unwind edges, `consteval_br`, `unreachable` | Same, minus `consteval_br` | Target branches, call-site tables |
| UB | Every UB and every non-constant operation is detectable by the interpreter | Poison for deferred UB, immediate UB at pinned operations (§6.1) | None introduced |
| Who runs it | Interpreter (constant evaluation, whole programs) | Optimizer, translation validator; a testing interpreter later (§13) | Encoder, object writer |

L1 is target-*parametrized*, not target-independent: `sizeof`, layout and ABI facts are already
needed by constant evaluation, and they arrive through the `layout`, `vtable` and `config`
queries (Part D). L2 erases C++: access paths become byte offsets from `layout`, virtual calls
become a load of the vptr and an indirect call, lifetime markers become stack-slot liveness,
`consteval_br` takes the runtime branch, `is_const_eval` becomes `false`, coroutines are split
into frame and resume functions, EH is explicit calls to the runtime with unwind edges.

L2 integer types don't stop at the register width. A 128-bit integer is an `i128` on every target,
and `legalize` (`src/ir/legalize.cppm`), an L2-to-L2 pass, expands the operations the target lacks:
into 64-bit halves, or a compiler runtime call. `Int128Support` lists what a target has. It is
empty for AArch64 and x86-64. A target with 128-bit registers names its operations there and
selects them, and nothing before `legalize` changes. How a 128-bit value crosses a call is separate,
the calling convention's (`LowerConfig::int128_in_words`). `legalize` runs in the `lower` cell
today, so that a body names its runtime symbols; it moves after the optimizer when rules on `i128`
exist. (#1172)

Why three and not two: the interpreter needs the typed, lifetime-aware view ([expr.const]), and
SMT needs the flat bit-vector view. Trying to serve both from one level would either burden the
e-graph with C++ object semantics or lose the information the evaluator needs. Why not more:
every extra level is a lowering that must be trusted (§6).

### 2.2 Shape: SSA, block arguments, explicit effects

- **CFG with block arguments** instead of phi nodes. The same information, but a value defined by
  a branch argument has no node that could be mistaken for a pure operation, and the e-graph treats
  block parameters as leaves without a special case.
- **State threading.** A `state` value (type `!state`) is passed into every effectful operation and
  returned by every operation that changes memory or performs I/O: `store(s, p, v) -> s'`,
  `load(s, p) -> v`, `call(s, …) -> (s', r…)`. One chain per function at first. Partitioning the
  state into independent chains by alias class is a later refinement that changes no interface:
  a `state` is a value like any other, and a `split/join` pair over it is two more operations.
- **Four node classes**, carried as attributes of each op in the op table:

  | Class | Position | Examples |
  | --- | --- | --- |
  | pure, speculatable | floats: placed by dominance of operands and uses (Click's global code motion) | arithmetic, bit ops, comparisons, `select`, casts, `ptr_add` |
  | pure, pinned | fixed to a block by a control operand | `sdiv`, `udiv`, `srem`, `urem` (trap on some targets, so not speculatable) |
  | state | pinned by its state operand and its block | `load`, `store`, calls, atomics, volatile, intrinsics with effects |
  | control | terminators | `br`, `switch`, `ret`, `call…unwind`, `unreachable` |

  Floating nodes are the e-graph's domain; pinned and state nodes are in the e-graph too, with the
  block as an operand, so they can be rewritten (store-to-load forwarding, dead stores, load CSE
  by congruence) but never move across control dependence. A pinned node built by a rule takes
  the block of the left-hand side's root pinned node, and a rule may not introduce a pinned node
  when its left-hand side has none. Extraction may choose a node with a block operand only if
  that block dominates every use of its class (§3.5).
- **Acyclicity.** Loop-carried values enter through block arguments, which are leaves, so the
  dataflow DAG of a function is acyclic. Loop transformations are structural passes on the
  skeleton (§3.1), not e-graph rewrites.

Rejected: a structured, loop-aware value graph (RVSDG, PEG) as the optimizer's form. It puts loops
inside the e-graph, but requires restructuring arbitrary control flow (`goto`, `switch` fallthrough,
EH), gives the interpreter and the debug-info mapping a second control model, and makes SMT
validation need loop invariants. The CFG skeleton plus floating DAG gets the e-graph's benefits for
everything acyclic now and leaves the door open: a `theta` node over a region would be one more
pinned node class. Also rejected: implicit memory dependence (recomputing alias edges per pass),
because it hides the effect structure the e-graph and the validator need, and phi nodes, as above.

### 2.3 What L1 carries for the interpreter

The constant evaluator must reject, as "not a constant expression" with a reason value, every
operation [expr.const] lists, and must detect UB. L1 makes each of these an explicit operation or
an attribute, so the interpreter never guesses:

| [expr.const] requirement | L1 device | Interpreter | Lowering to L2 |
| --- | --- | --- | --- |
| Lifetime ([basic.life]), access outside it | `obj.begin(p, T)`, `obj.end(p)` emitted at construction end and destruction start; `alloc` with storage duration | Abstract memory: objects with lifetime state per subobject, including an under-construction state for the accesses [class.cdtor] allows | Stack-slot liveness; dropped for globals |
| Uninitialized reads; erroneous behaviour (P2795) | `alloc(…, init = erroneous \| indeterminate \| zero)` | Bytes carry `uninit`/`erroneous`/value; reading either is non-constant | `erroneous` becomes a fixed byte pattern store (the implementation-defined value; zero on every target); `indeterminate` becomes nothing, reads yield poison |
| Pointer provenance, comparison, arithmetic ([expr.eq], [expr.rel], [expr.add]) | Pointers are `(object id, access path)`; `ptr_add(p, i, T)`, `ptr_cmp(op, p, q)`, `ptr_to_int`, `int_to_ptr` | Bounds and one-past-end checks; unrelated relational compares are non-constant; `ptr_to_int`, `int_to_ptr`, `reinterpret_cast` non-constant | Integers with provenance facts for alias analysis (§6.1) |
| Typed access, aliasing, unions ([basic.lval], [class.union]) | `load/store` carry the static type and the access path; `union.activate(p, member)` | Checks active member, dynamic type, path validity | Offsets from `layout`; `union.activate` dropped |
| Virtual calls, `dynamic_cast`, `typeid` | vptr is a typed field holding `vtable_addr(C)`; `vcall(s, obj, slot)` is a load plus `call` | Dispatch on the symbolic vtable value | Indirect call |
| Non-constant operations: non-constexpr calls, `volatile`, `asm`, `goto` into a block with initialization, `new` beyond the evaluation | Attributes on `call`, `load`, `store`; `asm` op; `alloc(dynamic)` | Reject with reason; transient allocation tracked and must be freed | Ordinary code |
| `std::is_constant_evaluated()`, `if consteval` | `is_const_eval` op; `consteval_br` terminator | By mode: `true` and the consteval edge in `constant` mode, `false` and the runtime edge in `runtime` mode | `false`; the runtime edge |
| Unsequenced conflicting side effects ([intro.execution]) | `useq.begin/end` markers around the operands of an unsequenced evaluation group | Records accesses inside a group; a conflict is UB | Dropped |
| Signed overflow, shifts, division ([expr.arith]) | Ops carry `nsw`/`nuw`/`exact` flags | Overflow is UB (non-constant) | Flags become poison-producing (§6.1) |
| Reflection ([meta.reflection]) | `meta.*` intrinsics on `std::meta::info` constants | Answered by the queries of §B7 | Never reach L2: a reflection value escaping to runtime is ill-formed |

The interpreter is a trampolined step loop inside a `query_task`: it fetches callees'
`function_ir` lazily at call sites with `co_await`, polls `stop_point()` every fixed number of
steps, and keeps a deterministic step budget ([implimits]). The same loop runs whole programs
(DECISIONS §4.6) with a host interface for I/O. It has two modes, `constant` and `runtime`, which
differ in which non-constant operations are permitted and in how `is_const_eval` and
`consteval_br` are answered: `true` and the consteval edge in `constant` mode, `false` and the
runtime edge in `runtime` mode. Whole-program runs and the optimizer's call folding (§3.2) use
`runtime`.

### 2.4 Identity, hashing, debug info

- `function_ir(F, ctx)` produces an interned, immutable L1 body: blocks in reverse post-order,
  nodes in a canonical order (dominance, then operand order, then the front end's emission
  order as a stable tie-break), every reference to another entity by entity id, every type by
  canonical type id. Node ids are dense indexes in that order. The fingerprint is the Merkle
  hash of this serialization and contains no `Loc`. Two contexts producing the same body share
  every downstream result.
- `function_debug(F, ctx)` is a separate value: node id → `Loc`, variable declaration → the nodes
  and `alloc`s that hold it, inlined-at chains later. Every transformation from L1 to L3 carries
  an **origin map** (new node → set of `(function entity, node id)` pairs) inside its value; `tu(TU)` composes the
  maps and renders DWARF late, so neither the IR fingerprint nor any optimization key ever sees a
  position (Part D).
- The **optimization unit** is what `ir_fp` in §A8 names: an interned value holding the function's
  L2 IR, the L2 IR of each callee in its *inline candidate set* (callees under a size threshold
  from `options`, taken transitively to a fixed depth, outside the function's SCC), and the
  `ipo_summary` of every other callee. Only definitions the link is sure to use are candidates:
  same TU, internal linkage, or `inline`/COMDAT definitions under the ODR (whole-program mode
  aside). `unit(F, ctx, inline_opts)` is a query; its fingerprint is a Merkle hash over
  that closure, so the optimization-cache key is honest about everything inlining can see, while a
  change to a large callee's body changes only its summary fingerprint, and then only if the
  summary changed.
- `ipo_summary(SCC)` (purity, memory effects, argument capture, return ranges) is a query keyed by
  the call-graph SCC that iterates to a fixpoint internally (Part D). Calls into the same SCC use
  the summary, never the body.

## 3. Equality saturation

### 3.1 Where

At L2, per optimization unit, over the whole function's dataflow after the structural passes of
that level (inlining, SROA/mem2reg, loop passes, CFG simplification) have run. Those passes are
ordinary code on the skeleton and are *searched over* at level 2 and above: a small fixed set of
pass orders is tried, saturation running once after each, and the cheapest estimated result is
kept, deterministically. The e-graph holds
every node of the DAG; block arguments, function arguments and the results of opaque calls are
leaves. A unit is one e-graph; units are independent and run under `par_map`. There is no shared
mutable e-graph across tasks (Part D), and no parallelism inside one.

### 3.2 Rule language

Rules are data, not code: `opt/rules/*.rules`, embedded with `#embed` and parsed by a `consteval`
parser into tables, so adding a rule is a one-line change and every rule has a name the evidence
can cite.

```
rule mul_pow2       : (mul ?x (const ?c)) => (shl ?x (const (log2 ?c)))   where is_pow2(?c)
rule store_load_fwd : (load ?s2 ?p) => ?v                                 where ?s2 = (store ?s ?p ?v), same_width
rule dead_store     : (store (store ?s ?p ?a) ?p ?b) => (store ?s ?p ?b)  where same_width
analysis known_bits : join = meet-of-bits, transfer per op                 ; drives `where` conditions and constant folding
```

A rule is a bidirectional equivalence unless marked `=>` only; typed variables range over all
widths unless restricted; side conditions are predicates on constants and e-class analysis facts
(known bits, ranges, non-zero, alignment), never on positions. Constant folding is one built-in
"rule": any node whose operands are all constants is evaluated by the L2 evaluator (the same op
semantics the interpreter uses after lowering) and merged with its constant. Calls to functions
the `ipo_summary` proves pure, with constant arguments, are folded by running the interpreter in
`runtime` mode on the callee's L1 (`function_ir`), with the L2 constants converted at the
boundary: no rule needed, and the result is as trusted as the interpreter already is.

### 3.3 Proving rules once

`tools/rules prove` translates every rule to an SMT query over the L2 semantics of §6.1 (QF_BV;
QF_ABV for memory rules) at widths 1, 8, 16, 32 and 64, under its side condition, and asks for a
counterexample to *equivalence* (not refinement, §3.4). Two sides are equivalent when, on every
input where neither side's pinned operations have immediate UB, their values and output states
are equal, and their UB conditions are equal except for UB already implied by the UB of a state
operand appearing on either side (such an operand is a pinned node that stays in the program, as
the store `?s2` in `store_load_fwd` does); `ptr` values must agree in
provenance too (§6.1). A rule that fails this goes to the directed pre-pass. The output is `opt/rules/PROOFS`: the rule
set's fingerprint, the solver build, and per rule `proven`, `trusted(reason)` or `rejected`. The
rule set fingerprint plus the PROOFS fingerprint is a component of `optimizer_version`; CI
re-proves when either changes; a rule without a `proven` or `trusted` row is not loaded.
`trusted` is for rules outside the solver's reach (a few floating-point rules under the strict
model, nothing else at first) and each one is listed in STATUS. Fixed widths are a known
limitation (Alive proves at fixed widths too); a rule is applied only at widths it was proven
at, so `__int128` and vector widths need their own entries in the list.

### 3.4 Only equivalences go in the e-graph

An e-class asserts equality in both directions. A rule whose right-hand side is merely a
*refinement* of the left (more defined, as `(x +nsw y) -nsw y => x` is: the left is poison on
overflow, the right is `x`) cannot be merged, or congruence would let another rule use it in the
wrong direction. So:

- The e-graph works over the **total algebra**: operations with poison as a value in the domain.
  `mul x 2 = shl x 1`, `add x 0 = x`, `load (store s p v) p = v` are equivalences there.
- **E-class analysis facts** (ranges, known bits, non-zero) that side conditions consult come
  only from flow-insensitive sources: the node's own operation and flags, constant operands,
  widths and extensions, argument attributes and `ipo_summary` return ranges. An e-class holds
  for the whole function, so a fact that holds only where something dominates (a guard, a
  dereference) is not an e-class fact; those facts are used by the directed pre-pass below. A
  fact derived from a flag is encoded in the proof as `¬poison(v) → P(v)`, and the rule must
  still be an equivalence over the total algebra, so a rewrite that only refines poison is
  rejected.
- Refinement-only rewrites (dropping a flag, replacing a poison-producing expression by a
  defined one, `freeze` elimination) run as a **directed pre-pass** before saturation, where
  direction is preserved, and the pass is covered by region translation validation (§6.2).

Rejected: direction-aware or "colored" e-graphs for refinement. Published, promising, and a
later experiment; not the foundation.

### 3.5 Extraction

Cost of an e-node: the measured cost (§8) of the cheapest L3 pattern that covers it on this
target, weighted by the static or profile frequency of its block (for a floating node, the
shallowest block its operands allow), as a positive integer with weights clamped to at least 1,
so greedy extraction cannot pick a cycle on a tie; leaves cost zero. Both extractors respect the
dominance restriction on pinned nodes (§2.2): a filter in greedy, one constraint per such node in
the ILP.

| Level | Extractor | Why |
| --- | --- | --- |
| 1, 2 | Greedy bottom-up (egg's extractor): each class takes its cheapest node given its children's costs, iterated to a fixpoint | Linear, never selects a cycle, good enough when sharing is modest |
| ≥ 2, when the pruned e-graph is under the level's size bound | ILP through the constraint layer (HiGHS): one Boolean per e-node, the root class and every child class of a chosen node must be covered, shared nodes are counted once, minimize the weighted cost | Greedy overcounts DAG sharing and cannot trade a cheap node here for an expensive one there (Goharshady et al.; TENSAT's formulation) |

The ILP model is built after **dominance pruning**: an e-node is dropped when another e-node of
its class is no more expensive and uses a subset of its child classes, and e-nodes with their own
class as a child are dropped. Cycles are forbidden by ordering integers (Miller–Tucker–Zemlin)
only on edges inside strongly connected components of the class graph. To make the result
independent of the solver's search path, the objective is all-integer: real costs are scaled by
`n · max_id` and a tie-break by e-node id is added, so every tie-break difference is at least 1,
the total stays below 2⁵³, and the optimum is unique. If the ILP hits its node or iteration budget
without an optimal solution, the best feasible solution found is used if one exists (it is a
valid extraction; feasibility is checked, §6.3), otherwise the greedy extraction.

**What S1 settled** (`spikes/optimization/egraph/`, a toy e-graph over i64 arithmetic with 13
rules):

| Question | Measured |
| --- | --- |
| Greedy against ILP | Greedy was worse on 40 of 50 random expressions: 8.7% higher DAG cost in total, 25.7% at worst; hand-made sharing examples 13 vs 11, 33 vs 23, 32 vs 24 |
| ILP solve time | Mean 0.22 s at ≤ 300 e-nodes, 0.88 s at 600, 3–14 s at 1,000; without dominance pruning the 300-node models took 2.4 s |
| Cycles | Without the ordering constraints 17 of 50 optimal picks were cyclic, so not programs |
| Growth | A 120-node input grew 255, 553, 2,031, 14,074, 235,733 e-nodes per iteration, then hit a 2M cap; none of the inputs saturates, so the budget decides the result |
| Determinism | Matching in id order: one fingerprint over five runs with different hash seeds and bucket counts, and the same under libc++. Matching in hash-table order: four different results in five runs. Parallel matching at 1, 2 and 8 threads: identical, and no faster, because rebuilding dominates |
| HiGHS | Repeatable for a fixed thread count and seed; a different thread count or seed gives a different solution of equal cost, and the default thread count depends on the machine |

So exact ILP is a small-region tool: the size bounds in §7 are a few hundred pruned e-nodes, and
larger e-graphs use greedy until a partitioned or treewidth-based extractor (Goharshady et al.)
is tried (§13).

### 3.6 Budgets and determinism

Budgets are the vector `(iterations, max_nodes, max_classes, matches_per_rule_per_iteration)` from
the ladder (§7). Saturation stops at the first limit reached, and that is a deterministic function
of the input. Determinism of construction and extraction rests on these rules, which S1 confirmed:

- Nodes are inserted in the IR's canonical order; ids are insertion order; the union-find root is
  the smaller id.
- Each iteration collects all matches first, stored per rule by position and sorted by (rule
  index, class id, node id), then applies them in that order, checking the node budget before
  each application, then rebuilds. Hash-cons tables are probed with exact keys and never
  iterated.
- Analysis facts are joined by a deterministic lattice meet; the rebuild worklist is processed in
  id order.
- Greedy extraction iterates classes in id order with ties broken by node id; the ILP's variable
  order is node order; HiGHS is given `threads = 1`, `random_seed` from the key fingerprint and
  `mip_rel_gap = 0` and `mip_abs_gap = 0` explicitly, never `time_limit`, since its defaults depend on the machine.

## 4. The constraint-model layer

### 4.1 Interface

`ycxx::cm` is the only way ycxx talks to a solver. It is deliberately a *flat* language, as FlatZinc
is to MiniZinc: the optimizer builds a `Model`, the layer hands it to a back end or writes it out.

```cpp
export module ycxx.cm;
export namespace ycxx::cm {
struct Model {
  IntVar  int_var(std::int64_t lo, std::int64_t hi, Name = {});
  BoolVar bool_var(Name = {});
  void linear(std::span<const Term> terms, Rel rel, std::int64_t rhs);   // Σ aᵢxᵢ ⋈ c
  void all_different(std::span<const IntVar>);
  void element(IntVar index, std::span<const std::int64_t> table, IntVar value);
  void table(std::span<const IntVar>, std::span<const Tuple>);
  void no_overlap(std::span<const Interval>);                           // disjunctive
  void cumulative(std::span<const Interval>, std::span<const IntVar> demands, std::int64_t capacity);
  void implies(BoolVar, Constraint);                                    // half-reification
  void minimize(std::span<const Term>);
};
struct Budget { std::uint64_t units; };                     // conflicts, nodes, iterations: back end's unit
struct Outcome { Status status;                             // optimal | feasible | infeasible | budget_exhausted | stopped
                 std::vector<std::int64_t> values; Stats stats; };
template<class B> concept Backend = requires(B b, const Model& m, Budget bu, Seed s, std::inplace_stop_token st) {
  { b.solve(m, bu, s, st) } -> std::same_as<Outcome>;       // synchronous leaf, no threads, no clock
  { B::supports(m) } -> std::same_as<bool>;                 // which constraint kinds it takes natively
};
void write_flatzinc(const Model&, Sink&);                   // for minizinc, chuffed, cp-sat offline
}
```

Contracts state the leaf's rules: `solve` is `pre(bu.units > 0)` and `post(r: r.status !=
feasible || satisfies(m, r.values))`. A back end that does not support a constraint kind gets it
decomposed by the layer (linearization, Boolean encodings); the decomposition is deterministic
and part of the layer, so the model a solver sees is a function of the model built. The stop token is wired
to the solver's own termination hook (CaDiCaL's terminator, a HiGHS callback, Z3's interrupt,
a Gecode `Stop`) and is used only to abort: a `stopped` outcome carries no values. The enclosing
`query_task` calls `co_await cx.stop_point()` between solver calls and after a `stopped` outcome;
a solver never observes the clock or a thread id (Part D).

### 4.2 Back ends, in order

| Order | Solver | Role | Licence, dependencies | Deterministic mode and unit |
| --- | --- | --- | --- | --- |
| 1 | **HiGHS** | ILP extraction (§3.5), ILP register allocation and scheduling models, LP relaxations for cost estimates | MIT; no required dependencies | Single thread; `mip_max_nodes` + `simplex_iteration_limit`; built with strict FP (no contraction, no reassociation) so pivoting is IEEE-deterministic across hosts; a CI test compares solutions between macOS arm64 and Linux x86-64 (S2 ran on one host only, so this stays open). S2: builds against libycxx, but presolve crashes on real instances until libycxx's B1 is fixed |
| 2 | **CaDiCaL** | Pure Boolean models: pattern covering (§5.1), rule-proof helpers, incremental "is this schedule feasible" checks | MIT; none | Single thread; conflict limit; fixed seed |
| 3 | **Z3** | Translation validation (§6.2), offline rule proofs, superoptimizer counterexamples | MIT; none required (own bignums) | Single thread; `rlimit` (a deterministic resource count); fixed seed; no time limit |
| 4 | **Gecode** | CP models for joint register allocation and scheduling (§5.2) with `cumulative`/`no_overlap` native | MIT; GMP/MPFR only for float variables, which are disabled | Sequential search; node and failure stops |
| export | **Chuffed**, **MiniZinc**, **OR-Tools CP-SAT** | Offline comparison through `write_flatzinc`; CP-SAT also as a candidate in-process back end later | Chuffed MIT; CP-SAT Apache-2.0 but pulls Abseil and Protocol Buffers | Chuffed: node limit; CP-SAT: `num_workers = 1`, `max_deterministic_time` |
| offline only | **Bitwuzla** | Second opinion on rule proofs | MIT, but GMP and MPFR (LGPL) are mandatory: not shipped (S2: no GMP-free build exists) | Wall-clock and memory limits only, so no deterministic budget (S2) |

Why this order: HiGHS unblocks the first non-greedy thing (extraction) and has no dependencies;
CaDiCaL is tiny and deterministic by construction; Z3 is the one SMT solver on the candidate list
with no LGPL dependency, and translation validation is the verification boundary's main tool, so
it comes before any CP solver; Gecode arrives with the combinatorial code generator, the first user
of global constraints. Kissat is not taken: it has no incremental API and CaDiCaL covers the same
ground. The shipped binary links HiGHS, CaDiCaL, Z3 and Gecode as `deps/` submodules built against
libycxx (DECISIONS §7.3). ycxx produces correct code with none of them
present: every solver path has a heuristic fallback (§5, §6.4).

**What S2 settled** (`spikes/optimization/solvers/`). CaDiCaL, Z3, Gecode, Chuffed and Bitwuzla
build with GCC 16.2 against both libstdc++ and libycxx and pass smoke tests. HiGHS builds both
ways, but against libycxx its presolve crashes on real instances because of a libycxx bug
(`set::end()` changes on the first insertion, B1), and one GCC 16.2 ICE needed a one-line
workaround (B2); both go upstream to libycxx before the HiGHS back end lands. Z3's `rlimit` count
was identical across the two standard libraries. CaDiCaL was repeatable per binary but took a
different search path under each standard library (52,405 against 44,220 conflicts on one
instance). So a deterministic budget is reproducible only for one solver *binary*: the solvers
are linked into ycxx, their build is covered by the build hash in `optimizer_version` (§7), and
`smt_validated` evidence names the solver build (§6.3). The cross-host CI test above compares
solutions for all four back ends, not only HiGHS, and the remedy of §6.3 applies to whichever
fails. Chuffed has only a wall-clock
limit and a time-based default seed, which confirms it as export-only.

## 5. Combinatorial code generation

Both paths read the same **target description**: a table of patterns `(L2 pattern, L3 instruction
template, operand constraints, cost class, size)` written in the rule language of §3.2, plus
register classes, calling-convention rules and encodings. Nothing in the back end emits an
instruction except through this table.

### 5.1 Instruction selection

- **Heuristic path (always present):** greedy maximal-munch tree matching over the dataflow DAG,
  trees cut at shared nodes, per block, in canonical order. This is the M5 selector.
- **Covering path (level ≥ 3):** choose a set of pattern instances such that every L2 node is
  covered exactly once and operands line up, minimizing weighted cost. Modelled as a Boolean
  covering with linear costs (CaDiCaL with a cardinality encoding when costs are uniform, HiGHS
  otherwise), after Koes and Goldstein; with Gecode when the model grows to include the operand
  and location constraints of universal instruction selection (Hjort Blindell et al.).

### 5.2 Register allocation and scheduling

- **Heuristic path:** M5 begins with every SSA value in a stack slot and a block-local
  allocator (`-O0`-like, debuggable). Level 1 adds SSA-based linear scan (Wimmer and Franz), with
  Braun and Hack's spilling and Boissinot et al.'s out-of-SSA translation, then list scheduling
  per block using the cost model's latencies.
- **Combinatorial path (level ≥ 3):** the joint model of Castañeda Lozano, Carlsson, Drejhammar
  and Schulte: a live range is an interval in a register-file "space × time" plane; `no_overlap`
  keeps ranges apart, `cumulative` bounds issue width and pressure, copies and spills are
  optional operations with costs, and the objective is weighted cycles or size. Solved per
  function with Gecode (or per block as a decomposition when the budget is small), exported to
  FlatZinc for offline comparison with Chuffed and CP-SAT. The heuristic solution seeds the search
  as an initial bound.

Both paths end in the same two **validators** (§6.3), which is the point: a solver's answer is as
trusted as the heuristic's, because in debug and CI builds neither is trusted.

## 6. The verification boundary

### 6.1 Semantics the validation stands on

Validation is only as good as the IR's semantics, so these are fixed here, for L2:

- **Poison** is the deferred-UB value (Lee et al., "Taming Undefined Behavior in LLVM"): produced
  by `nsw`/`nuw`/`exact` violations, out-of-range shifts, `ptr_add inbounds` leaving its object,
  reads of `indeterminate` memory; it propagates through pure operations, except that `select`
  is poison only if its condition or the chosen arm is poison, and a block argument takes only
  the taken edge's value; `freeze` stops it. `freeze` is never duplicated or rematerialized,
  since two copies may differ.
  Branching on poison, storing poison to memory that is later read as a pointer, or passing it to
  a pinned operation that requires a defined value is **immediate UB**.
- **Immediate UB** happens only at pinned operations: a memory access through a pointer that is
  not dereferenceable at that size, division by zero, a call to a `noreturn`-violating function,
  `unreachable`. Because only pinned operations have immediate UB and pinned operations never
  move, floating a pure node earlier is always sound: the worst it can produce is poison that
  nobody branches on.
- **No `undef`.** Uninitialized automatic storage is erroneous (a concrete pattern, §2.3) unless
  `[[indeterminate]]`, in which case reads give poison; uninitialized dynamic storage is
  indeterminate ([basic.indet]), so its reads give poison too. Memory holds bytes with a poison
  bit each.
- **Pointers** are integers with a provenance *fact*, maintained by alias analysis following the
  PNVI-ae-udi model (Memarian et al.; ISO/IEC TS 6010): a pointer derived from an allocation by
  `ptr_add` has that allocation's provenance; `ptr_to_int` *exposes* the allocation;
  `int_to_ptr` yields a pointer that may alias any exposed allocation, and a pointer loaded from
  memory may alias any allocation. Pointer equality is integer equality (unspecified results in
  the draft become concrete, as they do at run time), but two `ptr` values are *equivalent* only
  if their integers and provenance agree.
- **Refinement**, between a source and a target region: for every input, if the source has UB the
  target may do anything; otherwise the target has no UB, and for each output, the source's value
  is poison or equals the target's. This is the relation Alive and Alive2 check; it is the
  relation every transformation from L1 onward must satisfy, and the SMT encoding follows theirs:
  a value is a pair (bits, poison flag), UB is a Boolean, memory is a byte-addressed array.

Rejected: immediate UB for arithmetic (CompCert-style). It forbids the floating DAG: a pure node
could not be hoisted past a branch. Rejected: `undef`; Lee et al. show the two-value scheme is
unsound in combination with poison and gains nothing here.

### 6.2 What is proven, what is validated, what is trusted

| Transformation | Status | How |
| --- | --- | --- |
| Front end → L1, `lower` L1 → L2, coroutine splitting, EH lowering | Trusted | Differential tests: the interpreter runs L1 on the own suite and libycxx's; a flat-memory L2 interpreter later runs the lowered form on the same tests (§13) |
| Interpreter | Trusted, it *is* the semantics | Draft examples, differential tests against Clang and GCC program output |
| E-graph rewriting with proven rules, greedy or ILP extraction | **Correct by construction** given the rule proofs; the e-graph code is tested in `--verify-opt` builds by producing an explanation (a rewrite chain, Flatt et al.) for every extracted term and replaying it rule by rule | Evidence: `proven_rules{ruleset_fp}` |
| Directed pre-pass (refinement rules), SROA, mem2reg, CFG simplification, GCM placement, peepholes at L2 | **Translation-validated** per acyclic region when the level's TV budget allows; otherwise trusted | Z3 refinement query over §6.1; region = a block or a single-entry acyclic hammock; inputs symbolic; memory an array; alias facts as hypotheses |
| Inlining, loop passes (unrolling, LICM, induction variables), ABI lowering | Trusted in v1 | Tested; v2 adds pass-supplied witnesses (simulation relations) checked by SMT, after Namjoshi and Zuck |
| Instruction selection (patterns) | Patterns trusted; the covering checked | Each pattern's L3 semantics is executable (the L3 evaluator, §13) and tested against its L2 pattern on random and exhaustive small inputs; covering validity (every node once, operands aligned) is checked |
| Analyses that supply facts or TV hypotheses (alias analysis, e-class analyses, `ipo_summary`) and the L2 evaluator used for folding | Trusted | Differential tests against the L2 interpreter and the SMT semantics of §6.1; a wrong alias fact can let a wrong transformation pass TV, which is why these are listed |
| Late L3 passes: out-of-SSA copies, stack-slot colouring, block layout, prologue and epilogue, post-allocation peepholes | Trusted, except where the allocation validator reads their result | Tested |
| Register allocation | **Checked**: solver answers in every build; the heuristic allocator in debug and CI builds, trusted in release builds | Validator after Rideau and Leroy: a dataflow check that every use reads the value the pre-allocation code defined, spills and reloads included; no SMT |
| Scheduling | **Checked**, on the same terms as allocation | Same multiset of instructions, every dependence edge of the (trusted) dependence graph respected |
| Superoptimizer results | **Translation-validated** | The SMT query is part of the search |
| Encoding, object writer, linker | Trusted | Round-trip tests through our disassembler of our own objects (DECISIONS §2.3) |

Why not validate everything with SMT: it costs a solver call per region per optimization run,
so the ladder affords it from level 2 only, bounded to small changed regions (which `-O2` compile
time pays for), and fully only at level 3 and above; proofs once per rule and cheap checkers per solver
answer give the same guarantee for a fraction of the cost, and the trusted set is short and named.

### 6.3 Evidence as a value

`optimized`'s value carries, per region it changed, one of:

| Kind | Content | Replay re-check |
| --- | --- | --- |
| `proven_rules` | rule set fingerprint | the manifest's fingerprint equals the running optimizer's |
| `smt_validated` | fingerprint of the SMT query, solver build id, `rlimit`, result `unsat` | re-run the same query with the same solver build and budget; deterministic, so the answer is the same; `--replay-trust` skips it for speed |
| `checked` | which validator (feasibility, allocation, dependences) and its input fingerprint | re-run the validator (microseconds) |
| `trusted` | the pass name | none; this is what the trusted list means |

A re-check that fails under `--replay` is a hard error (the manifest does not match this
compiler), never a fallback, since a fallback would change the output the manifest promises.

A solver's *solution* is itself part of the value (the extraction chosen, the allocation, the
schedule), so a replay never re-solves: it re-checks. This is also what makes §A8's "same key,
same value on any machine" survive HiGHS: if the cross-host FP determinism test of §4.2 fails,
HiGHS moves from the synchronous levels to the anytime layer, where results travel through the
optimization cache and manifests carry the solution, and a fresh build on another machine may
differ while remaining correct, which is the trade §A8.3 already accepts.

### 6.4 Failure and exhaustion

- A validator that fails or an SMT query that returns `sat` (a counterexample) or `unknown` is a
  **bug report**, not a slowdown: the region keeps its pre-transformation form, the result's
  evidence records `fallback{reason}`, and the daemon writes the counterexample to its log for
  the oracle's work list (§9).
- Budget exhaustion in a solver or saturation uses the heuristic result for that region, and the
  value records it. This is deterministic because budgets are (§A8.1).
- Cancellation never produces a result: every solver leaf is followed by `co_await
  cx.stop_point()`, and a synchronous leaf returns `stopped` which the coroutine turns into that
  await (Part D).

## 7. Budgets and the optimization cache

`optimized(unit_fp, target, options, optimizer_version, level)` is §A8.1's query; this section
refines its ladder. `options` holds everything that changes *semantics or objective*
(`-ffast-math` subsets, `-Os`/`-Oz` weighting, `-fno-exceptions`, PIC, profile data fingerprint);
`level` holds only budget. `-O0`, `-O1`, `-O2`/`-O3` request levels 0, 1, 2 synchronously;
background work continues up the ladder. As a refinement of §A8.3, `-O0` pins level 0 exactly:
`best_available` is consulted only for `B ≥ 1`, so a debug build never receives optimized code
from the cache. The self-hosting fixed point (DECISIONS §3.4) and the incremental == cold tests
run with a pinned snapshot `S` (empty or a manifest), since background work between two runs
changes `S`.

| Level | Passes | E-graph (iters, nodes, classes, matches/rule) | Extraction | Inlining growth | Codegen | TV `rlimit`/region | Superopt |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | none: `lower`, tree select, stack slots | off | – | none | heuristic | off | off |
| 1 | mem2reg/SROA, const fold, DCE, CFG simplify, directed pre-pass | 4, 10k, 5k, 100 | greedy | none | linear scan, list schedule | off (trusted) | off |
| 2 | + inlining by cost, loop passes, pass-order search over 3 orders | 12, 100k, 50k, 1k | greedy; ILP (10k B&B nodes, 100k iters) when the pruned e-graph has ≤ 300 e-nodes | 2× unit size | same | 10⁶ on changed regions ≤ 64 nodes | off |
| 3 | + combinatorial selection, joint RA/scheduling | 24, 400k, 200k, 4k | ILP (10⁵ B&B nodes, 10⁶ iters) when ≤ 1,000 pruned e-nodes | 4× | CP: 10⁵ failures per function; SAT: 10⁵ conflicts | 10⁷ | 10³ candidates, regions ≤ 6 nodes |
| 3+k | same | every numeric budget × 4ᵏ | | | | | |

The numbers are starting points, tuned on the corpus (#9) and committed as data in
`opt/ladder.toml`; the file's fingerprint is part of `optimizer_version`, so retuning is a version
bump, never a silent change.

The optimization cache, `best_available(key, B, S)` and the replay manifest are exactly §A8.2–A8.4.
Two refinements: the OC entry is `(unit_fp, target, options, optimizer_version) → (level, result,
evidence, cost estimate)`, with the whole result and not only its fingerprint, since a result is
reused on machines that never computed it; and `optimizer_version` is the fingerprint of (rule set,
PROOFS, ladder, pattern tables, and the build hash: the hash of the binary's build inputs, i.e.
the revisions of ycxx and every `deps/` submodule, libycxx included, and the compiler's version
and flags), so any of those changing
invalidates the cache in O(1) by key.

## 8. Cost models

- **Measured:** per instruction class of each target: latency, reciprocal throughput, the set of
  execution resources it occupies (a resource vector, inferred when the vendor documents none),
  load-to-use latency per addressing mode, branch and call costs, encoded size. Methodology after
  Abel and Reineke (uops.info, nanoBench): microbenchmarks with dependency chains for latency and
  independent streams for throughput; port mappings on undocumented cores inferred from
  throughput measurements as in Ritter and Hack (PMEvo) and Derumigny et al. (Palmed). Quality is
  checked BHive-style: predicted against measured cycles of basic blocks from the corpus. The
  harness is #9's "microbenchmarks" item.
- **Stored** as `targets/<arch>/<uarch>.costs`: one text file per microarchitecture, a header with
  schema version, machine, OS, tool version and date, then one row per instruction class. A
  `generic.costs` per architecture is derived by a stated formula (max latency, min throughput
  over the measured set) and committed too. Files are data: no constant in code.
- **Engine input:** `cost_model(target)` is an input kind of durability `High`; its fingerprint
  is a component of the `target` key of `optimized` (`target = (triple, uarch, ISA feature set, cost_fp)`), so a
  retuned table changes keys and never mixes with old results (Part D). Derived from it:
  `op_cost(L2 op, target)` = the cheapest covering pattern's cost, which is what the e-graph
  extractor and the inliner use, so mid-level costs and machine costs agree by construction.

## 9. The performance oracle and the work list

The oracle (#9) builds the corpus with ycxx, Clang and GCC at matching levels and attributes run
time to functions with the platform sampler, mapped back through debug info to entity ids and
then to `unit_fp`. From this the daemon keeps a **work list**, plain daemon state and never an
engine input: entries `(unit_fp, target, options, priority)` with priority = measured gap ×
sample share, plus a recency boost for functions edited in this session, plus explicit requests
(`ycxx --optimize F`, an LSP code action). Idle workers pop the list and run `optimized(…,
level + 1)` as `background` roots (§A3.5); results land in the OC at quiescence. Order of the
list changes only *which* cells exist, never any value.

For humans and agents the same data is a report: per benchmark the gap, per hot function the
estimated versus measured cost (a cost-model bug when they disagree), the evidence kinds and
`fallback{reason}` entries (validator failures are bugs with a counterexample attached), and the
superoptimizer's verified `(region, cheaper form)` pairs in `opt/rules/CANDIDATES`, which an agent
generalizes into rules and proves (Ruler-style inference can automate the first step). Reading a
competitor's code for a specific gap follows DECISIONS §2.3 and is logged on the issue.

### 9.1 Evidence for an optimizer change

Use the same stored IR, target, cost model and initial heuristic for both sides of an
experiment. Change one pass, rule family, model encoding, or solver setting at a time.
Record the compiler and solver revisions, flags, host, seed, input identity, and
deterministic budgets with the raw results.

1. **Check correctness and applicability.** Verify the candidate using §6, compare
   outputs with the reference program, and count how often the candidate is usable
   across the corpus. Keep timeouts, exhausted budgets, and rejected candidates in
   the results.
2. **Measure optimizer work.** Use `ycxx::bench` for warmed local comparisons and
   record model construction, solving, and validation separately. Include the model
   size, solver work units, objective values, and achieved optimality. Report the
   total optimizer cost including fallbacks. Wall time describes the experiment.
   Deterministic work units govern solver budgets.
3. **Measure the generated code.** Use the oracle corpus (#9) to compare runtime and
   code size, along with cold and incremental compile time. Use held-out workloads
   when selecting cutoffs or cost-model weights. Treat a better modeled objective
   as a candidate for runtime measurement. Use paired samples, retain their
   variation, and require a practical effect before reporting a speedup.

The first local evidence uses the same small e-graphs with and without dominated-node
pruning. That isolates pruning's effect on exact extraction work and objective
values. A separate benchmark measures FlatZinc serialization with model construction
outside the timed loop. [Build and reproduction commands](../BUILDING.md#optimizer-experiments).

On Linux x86-64 with GCC 16.2 and the pinned HiGHS 1.16.0, use seeds 1000–1009
with the 30-entry inputs and the 300-e-node cap from the spike. Run pruned, unpruned,
unpruned, then pruned. All 10 exact objectives matched across configurations, and exact
extraction lowered the aggregate modeled DAG cost from 705 to 614 versus greedy.
Pruning reduced the sum of branch-and-bound nodes from 18,170 to 261 per block.
The median of the ten within-case solve-time ratios was 15.72×. This measures
HiGHS solve work on the spike corpus. Generated-code runtime requires the corpus
comparison described above.

### 9.2 End-to-end solver experiments

Take one candidate transformation at a time through the existing `Optimized`
query. Hold the input, target, optimization options and baseline fixed, and
validate the selected result before codegen. Record model construction, solve
and validation work separately, then compare runtime, code size, and cold and
incremental compilation on the same workload. The extraction spike measures
the cost model and solver work; this experiment measures the generated program.

Expose the decision as child query nodes. Show the modeling path, number of
variables and constraints, backend, deterministic budget, selected result
and validation or fallback. Use the engine's worker identity and timing events
for interactive and JSON tracing. Solver choices and reproducibility keys
contain the semantic inputs, solver build and deterministic limits.

Schedule independent solve queries across the engine's existing workers. Each
solve owns its model, solver state and stop hook, and runs one solver thread.
Cancellation is propagated before and after the solver call. Tests compare final
program bytes at one and multiple workers, and check overlap of the solve nodes
themselves on independent work.

### 9.3 Unsigned division on x86-64

`-fycxx-optimize=heuristic` replaces eligible unsigned division and remainder by a
power of two with a shift or mask. `-fycxx-optimize=solver` also tries to fuse
compatible 32- and 64-bit unsigned quotients and remainders in the same block.
One hardware divide supplies both results. The emitter keeps operand evaluation
in place and saves the second result in its existing stack slot. The solver
leaves power-of-two divisors to the heuristic.

Group sites by equivalent operands and visit each group in instruction order.
A site selected as the later half of a pair consumes a distinct earlier site of
the opposite kind. In every prefix the number of selected sites can be at most
the smaller of the quotient and remainder counts. This also suffices for
reconstructing pairs. For each selected site, the objective counts the bytes
its omitted instructions would have taken. The x86 encoder supplies those costs.
`ycxx.cm` solves this nested prefix model exactly with a heap and retains
earlier sites on weight ties. It can express the same model as linear rows
for FlatZinc export.

Each function has a deterministic budget of 4,096 solver site visits. The
emitter checks pair compatibility and disjointness.
The validation query compares the actual function size with the predicted
savings over the heuristic emission. Budget exhaustion or failed validation
uses the heuristic. A stopped solve publishes no candidate.

Use `--trace-engine=interactive` or `--trace-engine=json` to inspect
`DivisionFusionModel`, `DivisionFusionSolve` and `DivisionFusionValidate`.
The events include worker and elapsed time. The solve result reports the budget,
visits and selected byte savings. The emitted result reports validation or fallback.

**Emitted-code evidence.** The [division spike](../BUILDING.md#benchmark-emitted-division-code)
compiles one pinned workload in both modes. On an x86-64 Xeon Platinum 8573C, three
fresh 50-round comparisons and three fresh 120-round comparisons measured 17–24%
less wall time and 18–24% less CPU time per operation for the 64-bit shard index.
Every 95% interval cleared the 1% practical threshold. The least favorable lower
bounds were 12% for wall time and 13% for CPU. Its emitted body shrank from 429 to
407 bytes. The 32-bit lane index shrank from 437 to 414 bytes, with a practical
timing win in four of six runs. Both modes emitted identical power-of-two page
offset code. In isolated shard processes, both modes had median resident and
peak RSS of 1,468 KiB across five fresh processes each.

In ten alternating compile pairs on the same fixture, cold medians were 32.2 ms
for the heuristic and 34.6 ms for the solver. Incremental medians after changing
one function were both 10.6 ms. Both paired intervals included zero. One cold
single-worker trace recorded 94 µs constructing models, 38 µs solving and 296 µs
validating. Validation emits both versions to check the size difference.

The corpus suggests this order for the next models.

1. Try sharing a single dynamic divide across more than two compatible results.
   Keep it only if elapsed and CPU time improve beyond the current pair fusion.
2. Try a size-sensitive choice between fusion and shift/mask for power-of-two
   divisors. The emitter prototype saved 15 function bytes on the page-offset
   case with inconclusive timing. Require a whole-function size reduction within
   the declared runtime tolerance.
3. Treat reciprocal constant division as an emitter experiment. The source-level
   multiply/shift recipe grew the body from 252 to 305 bytes and lost 17 of 18
   comparisons. Reconsider it only if a register-level recipe reverses that result.

## 10. What the first back end must build

Everything below exists so that the optimizer can be added without the front end, the IR or the
back end's shape changing. Nothing below requires an e-graph, a rule, a solver or an SMT query.

**M4 (IR and interpreter) must deliver:**

| Must exist | Why now |
| --- | --- |
| L1 as §2.1–2.3 define it: block arguments, state threading, the four node classes as op-table attributes, flags on arithmetic, typed access paths, `obj.begin/end`, `union.activate`, `alloc` with init kind, `is_const_eval`, `consteval_br`, `useq` markers, `ptr_*` ops, EH terminators, coroutine ops, `meta.*` intrinsics | The interpreter needs all of it for [expr.const]; the optimizer needs the node classes and flags; retrofitting state threading would rewrite the IR generator |
| `function_ir(F, ctx)` with canonical order, dense node ids, positionless fingerprint; `function_debug(F, ctx)` as a separate value | Part D's hashing rule; debug info by origin maps |
| The op table as one reflected C++26 definition: name, operand types, class, flags, L1 evaluator | One table serves the interpreter, the printer, the fingerprint, later the rule parser and the SMT encoder |
| The trampolined interpreter with lazy `co_await` callee fetch, `stop_point` polling, abstract memory with lifetime, init state and provenance; a textual IR printer and parser for tests | DECISIONS §4.6; tests |

**M5 (AArch64 Mach-O) must deliver:**

| Must exist | Why now |
| --- | --- |
| `lower(ir)` L1 → L2 as §2.1 describes, coroutine splitting (ramp, resume and destroy pieces get entity ids from their origin and an ordinal, and their own units) and EH lowering included, alias tags on memory operations | The optimizer sits on L2; the pipeline's shape is final on day one |
| `unit(F, ctx, inline_opts)` with an empty inline candidate set, `optimized(unit, …, level 0)` = `lower` + heuristic codegen, `codegen(F)` reading `best_available(key, B, S)` with `S` the empty `opt_cache_snapshot` input | The plumbing of §A8 runs from the first build, so anytime work later only adds cells |
| The target description as data: pattern table in the §3.2 language (read by the maximal-munch selector), register classes, calling convention, encodings | The covering selector reads the same table; nothing emits instructions outside it |
| Stack-slot allocation with a block-local allocator, then SSA linear scan and list scheduling; the allocation validator and the dependence checker, run on every function in debug and CI builds | The validators are the verification boundary for all later allocators and schedulers |
| L3 with an origin map; DWARF and unwind tables rendered late by `tu(TU)` from `function_debug` composed with origin maps | Debug info survives optimization by construction |
| `cost_model(aarch64)` as an input kind reading a stub `generic.costs` with the schema header | The key shape and the input exist; the numbers come with #9 |
| Deterministic symbol, section and function order from entity ids (§A5.3) | Part D |

**Deferred entirely** (each a row in §13): L2 interpreter, the rule language and prover, the
e-graph, `ycxx::cm` and every solver, translation validation, superoptimization, cost measurement,
the work list, OC population, manifests, inlining and loop passes, pass-order search.

## 11. Constraints on other designs

**On the AST design (#2).** IR generation (`function_ir`) consumes the analysed body and needs, as
explicit nodes or facts, not as things to re-derive:

1. Every implicit conversion, user-defined conversion call, and standard conversion sequence as
   explicit nodes with the chosen function, in the order [over.ics] gives.
2. Materialized temporaries as explicit nodes, each with its storage duration, its destructor and
   the point its lifetime ends ([class.temporary]), lifetime extension already resolved.
3. The cleanup structure: for each scope exit (normal, `return`, `break`/`continue`, `goto`,
   exception) the ordered list of destructor calls, including partially constructed objects
   ([except.ctor]).
4. The sequenced-before relation of each full-expression as a partial order over its
   subexpressions: sequenced, indeterminately sequenced, unsequenced ([intro.execution]). IR
   generation linearizes it and emits `useq` markers from the unsequenced groups.
5. Value categories and the implicit object parameter resolved; `this` adjustments, derived-to-base
   and base-to-derived steps as explicit nodes naming the classes, so `layout` and `vtable`
   supply offsets and slots.
6. Initialization fully decided: aggregate element order, default member initializers substituted,
   zero/value/default initialization explicit, constant initialization versus dynamic
   initialization decided per variable ([basic.start.static]), with the erroneous/indeterminate
   state of each automatic variable ([dcl.init.general], P2795).
7. For each function: constexpr/consteval, `noexcept` as a fact, coroutine body with promise
   calls explicit ([dcl.fct.def.coroutine]), `if consteval` as a distinct statement node, and for
   each expression whether it is manifestly constant-evaluated.
8. `std::meta::info` values as constants with an identity the interpreter's `meta.*` intrinsics
   can map to §B7's queries; reflections never reach runtime code.
9. Entity ids for everything IR references (functions, variables, classes, vtables, string
   literals, lambda closures, local classes); canonical type ids for every type; no positions in
   any of them.

**On the query engine (#1).** Nothing new is required. Used as specified: `opt_cache_snapshot` and
`cost_model` as high-durability inputs; `par_map` over units; `stop_point` in long leaves; the
seeded RNG derived from the key fingerprint (§A5.1); interned large values with Merkle
fingerprints for IR bodies and e-graph results. One expectation to confirm in the engine's first
commits: a `query_task` that interleaves many `co_await cx.fetch<FunctionIr>` with interpreter
steps (one per call site) stays within the frame-size budget of §A3.3, since the interpreter's own
state lives on its heap, not in the frame.

## 12. Rejected alternatives, collected

Each is argued where it is cited. RVSDG/PEG as the optimizer's form, and phi nodes (§2.2).
Immediate UB for arithmetic, and `undef` (§6.1). Refinement rules inside the e-graph, and colored
or directed e-graphs (§3.4). Rules as C++ code (§3.2); proofs at symbolic widths (§3.3). Bitwuzla
in the binary, Kissat, CP-SAT in-process first (§4.2). Time budgets and parallel solver modes
(§3.6, §4). SMT validation of everything (§6.2); SMT proof certificates as evidence, since
bit-vector proofs are huge or unavailable and a deterministic re-run is the certificate (§6.3).
`optimized` stopping at L2 with codegen keyed separately: combinatorial codegen is level-dependent
and must share the cache and the cost estimate (§7). Cost constants in code (§8). The work list as
an engine input (§9).

## 13. Follow-up implementation issues and open items

The first eight rows, what M4 and M5 need, are open as #23–#30. The rest are opened when M5
lands.

| Issue | Area | Content |
| --- | --- | --- |
| L1 IR definition and op table (#23) | `area:ir` | Reflected op table, block arguments, state threading, node classes, flags, access paths, lifetime and init ops, printer/parser, canonical order and fingerprint, `function_debug` |
| `function_ir` generation (#24) | `area:ir`, `area:sema-decl` | From the analysed body per §11; EH, coroutines, cleanups, static initializers |
| Interpreter (#25) | `area:interp`, `area:consteval` | Trampolined loop, abstract memory, [expr.const] checks, host interface for whole programs, step budgets |
| `lower` L1 → L2 (#26) | `area:ir` | Byte offsets from `layout`, vptr loads, coroutine splitting, EH lowering, `consteval_br`, erroneous-value stores |
| L2 flat-memory interpreter and L3 evaluator (#27) | `area:interp`, `area:tests` | Differential testing of `lower` and of pattern semantics |
| AArch64 target description and heuristic codegen (#28) | `area:codegen-aarch64` | Pattern table, maximal munch, stack slots then SSA linear scan, list scheduling, encodings |
| Allocation validator and dependence checker (#29) | `area:codegen-aarch64`, `area:tests` | Rideau–Leroy style checker; schedule checker; run in CI |
| `optimized` level 0, `unit`, `best_available` plumbing (#30) | `area:opt`, `area:query-engine` | The §A8 cells over an empty snapshot; `opt_cache_snapshot`, `cost_model` inputs |
| Rule language and prover | `area:opt` | `#embed` + consteval parser, SMT translation, `PROOFS`, CI job |
| E-graph engine | `area:opt` | Deterministic construction, analyses, rebuild, greedy extraction, explanations in `--verify-opt`; informed by S1 |
| `ycxx::cm` and HiGHS, CaDiCaL back ends | `area:solver` | Interface, decompositions, FlatZinc writer, deterministic builds; HiGHS waits on yrashk/libycxx#5 (B1) and yrashk/libycxx#1 (B2) (S2) |
| Z3 back end and translation validation | `area:solver`, `area:opt` | §6.1 encoding, region selection, evidence, fallback |
| Structural passes and pass-order search | `area:opt` | mem2reg/SROA, inlining by cost, loop passes, directed pre-pass |
| Gecode back end and combinatorial codegen | `area:solver`, `area:codegen-aarch64` | Covering selection, joint allocation and scheduling model |
| Superoptimizer | `area:opt` | Enumerative CEGIS over small regions, `CANDIDATES` output |
| Cost-model measurement harness | `area:bench` | Microbenchmarks, inference, `.costs` files, BHive-style validation (with #9) |
| Anytime layer | `area:daemon`, `area:opt` | Work list, background roots, OC writes at quiescence, `--export-opt-cache`, `--replay` |

Open items, settled in those issues:

- **State partitioning** by alias class (§2.2): when, and whether the e-graph needs it.
- **Region selection for TV** (§6.2): block versus hammock, and the size bound at each level.
- **Pass-order search space** (§3.1): which orders at level 2.
- **Floating point:** which rules are equivalences under the strict model, what `-ffast-math`
  subsets add to `options`, and whether Z3's FP theory or SymFPU-style bit-blasting proves them.
- **Profile input:** format and fingerprint of profile data in `options`.
- **HiGHS cross-host determinism** (§4.2): the CI test, and the move to the anytime layer if it
  fails.
- **Extraction beyond a few hundred e-nodes:** partitioned or treewidth-based exact extraction
  (Goharshady et al.), or ILP on windows of the greedy result. S1 showed exact ILP does not scale
  past about 1,000 pruned e-nodes.
- **Rule scheduling:** associativity and commutativity drive most of the growth in S1; a
  scheduler that backs off rules that match too often (as egg's paper describes) is the first
  thing to try.

## 14. Provenance

Following DECISIONS §6.5, the decisions (and a first draft of the text) came from a Fable
subagent. The spikes, the changes they forced (§3.5's size bounds, dominance pruning, the unique
optimum and HiGHS's explicit settings; §4.2's solver facts), the editing and the bookkeeping were
done by the Opus session that owns the issue. One independent review by a second Fable subagent
found 2 P0 (path-dependent facts used as function-wide e-class facts; the interpreter answering
`if consteval` as constant evaluation in whole-program runs), 13 P1 and 10 P2; the session
applied the smallest fix for each. The reviewer's check of that diff found one P1 and two P2 in
the fix text, each fixed as it proposed; that last fix was not re-verified.

Sources, per DECISIONS §2: the C++26 working draft (cited by stable clause name), P2795
(erroneous behaviour), ISO/IEC TS 6010 (pointer provenance), the accepted query-engine design,
and published papers only. No compiler's source code or design documents were consulted, and
nothing under `~/.local/share/ycxx/suites/` was opened. Papers relied on:

- Equality saturation: Tate, Stepp, Tatlock, Lerner, *Equality Saturation: A New Approach to
  Optimization* (POPL 2009); Willsey, Nandi, Wang, Flatt, Tatlock, Panchekha, *egg: Fast and
  Extensible Equality Saturation* (POPL 2021); Zhang et al., *Better Together: Unifying Datalog and
  Equality Saturation* (PLDI 2023); Flatt, Coward, Willsey, Tatlock, Panchekha, *Small Proofs from
  Congruence Closure* (FMCAD 2022); Yang et al., *Equality Saturation for Tensor Graph
  Superoptimization* (MLSys 2021); Goharshady, Lam, Parreaux, *Fast and Optimal Extraction for
  Sparse Equality Graphs* (OOPSLA 2024); Nandi et al., *Rewrite Rule Inference Using Equality
  Saturation* (OOPSLA 2021).
- IR shape: Cytron et al., *Efficiently Computing Static Single Assignment Form* (TOPLAS 1991);
  Click and Paleczny, *A Simple Graph-Based Intermediate Representation* (IR 1995); Click, *Global
  Code Motion/Global Value Numbering* (PLDI 1995); Reissmann et al., *RVSDG* (TECS 2020); Bahmann
  et al., *Perfect Reconstructability of Control Flow from Demand Dependence Graphs* (TACO 2015);
  Braun et al., *Simple and Efficient Construction of SSA Form* (CC 2013).
- Semantics and validation: Lee, Kim, Hur, Lopes, Regehr et al., *Taming Undefined Behavior in
  LLVM* (PLDI 2017); Lopes, Menendez, Nagarakatte, Regehr, *Provably Correct Peephole Optimizations
  with Alive* (PLDI 2015); Lopes, Lee, Hur, Liu, Regehr, *Alive2: Bounded Translation Validation
  for LLVM* (PLDI 2021); Memarian et al., *Exploring C Semantics and Pointer Provenance* (POPL
  2019); Pnueli, Siegel, Singerman, *Translation Validation* (TACAS 1998); Necula, *Translation
  Validation for an Optimizing Compiler* (PLDI 2000); Namjoshi and Zuck, *Witnessing Program
  Transformations* (SAS 2013); Rideau and Leroy, *Validating Register Allocation and Spilling* (CC
  2010); Tristan and Leroy, *Formal Verification of Translation Validators: Instruction Scheduling*
  (POPL 2008).
- Code generation: Koes and Goldstein, *Near-Optimal Instruction Selection on DAGs* (CGO 2008);
  Hjort Blindell, Castañeda Lozano, Carlsson, Schulte, *Modeling Universal Instruction Selection*
  (CP 2015); Castañeda Lozano, Carlsson, Drejhammar, Schulte, *Constraint-Based Register Allocation
  and Instruction Scheduling* (CP 2012) and *Combinatorial Register Allocation and Instruction
  Scheduling* (TOPLAS 2019); Castañeda Lozano and Schulte, *Survey on Combinatorial Register
  Allocation and Instruction Scheduling* (CSUR 2019); Wimmer and Franz, *Linear Scan Register
  Allocation on SSA Form* (CGO 2010); Braun and Hack, *Register Spilling and Live-Range Splitting
  for SSA-Form Programs* (CC 2009); Boissinot et al., *Revisiting Out-of-SSA Translation* (CGO
  2009); Wilken, Liu, Heffernan, *Optimal Instruction Scheduling Using Integer Programming* (PLDI
  2000); Malik, McInnes, van Beek, *Optimal Basic Block Instruction Scheduling for Multiple-Issue
  Processors Using Constraint Programming* (IJAIT 2008).
- Superoptimization: Massalin (ASPLOS 1987); Bansal and Aiken (ASPLOS 2006); Phothilimthana et
  al., *Scaling up Superoptimization* (ASPLOS 2016); Sasnauskas et al., *Souper: A Synthesizing
  Superoptimizer* (arXiv 2017).
- Cost models: Abel and Reineke, *uops.info* (ASPLOS 2019) and *nanoBench* (ISPASS 2020); Ritter
  and Hack, *PMEvo* (PLDI 2020); Derumigny et al., *Palmed* (CGO 2022); Chen et al., *BHive*
  (IISWC 2019).
- Solvers and modelling: Nethercote et al., *MiniZinc* (CP 2007); Ohrimenko, Stuckey, Codish,
  *Propagation via Lazy Clause Generation* (Constraints 2009); Huangfu and Hall, *Parallelizing the
  Dual Revised Simplex Method* (MPC 2018); Biere et al., *CaDiCaL 2.0* (CAV 2024); de Moura and
  Bjørner, *Z3* (TACAS 2008); Niemetz and Preiner, *Bitwuzla* (CAV 2023); Schulte, Tack,
  Lagerkvist, *Modeling and Programming with Gecode*. Licence, dependency and resource-limit
  statements are from the solvers' public documentation and build files, checked by S2.
