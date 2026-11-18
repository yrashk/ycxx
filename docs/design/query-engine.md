# Query engine, incremental invalidation and determinism

Design for [yrashk/ycxx#1](https://github.com/yrashk/ycxx/issues/1). Status: **proposed**. When
it is accepted, §1 is summarized into DECISIONS §4.1–4.5 and §4.8.

This is the foundation of ycxx: every analysis result is a memoized query, and everything else
(preprocessor, parser, semantic analysis, interpreter, optimizer, code generator, linker) is
written as queries on this engine. The document answers every question in the issue:

| Issue question | Where |
| --- | --- |
| Query model: inputs, keys, revisions, red/green, early cutoff, cycles | §A1, §A2 |
| C++-specific dependency recording | Part B (§B1–§B8, §B12) |
| Preprocessor | §B9 |
| Granularity, and the cost of a body edit and of a new overload | §B11 |
| Sharing across TUs; instantiations program-wide; ODR as a by-product | §A1.9, §B6, §B10 |
| Concurrency, cancellation, memory model, `<execution>` | §A3, §A4 |
| Determinism, diagnostics ordering and deduplication | §A5, §A1.7 |
| Memory, garbage collection, persistence | §A6, §A7 |
| Optimization as queries | §A8 |
| The incremental == cold invariant | §A9, §B12, Part C |

## Provenance and who decided what

Sources, per DECISIONS §2: the C++26 working draft (cited by stable clause name), WG21 papers,
and published work on incremental computation: salsa's red–green validation and durability as
publicly described, Adapton, Mokhov, Mitchell and Peyton Jones, *Build Systems à la Carte*
(ICFP 2018), Roslyn's red–green trees, Blumofe–Leiserson work stealing and the Chase–Lev deque.
No compiler's source code or design documents were consulted. The GCC 16.2 feature probes behind §A10.4 were
compiled in a scratch directory against libstdc++; only their accept/reject and run results were
used (DECISIONS §2.3). The issue's prior-art list also
names Zig's incremental compilation writeups; DECISIONS §2.2 forbids design documents of Zig's
compiler, so they were not read.

Following DECISIONS §6.5, the design reasoning was done by two Fable subagents. The writing,
the review, the spike and the synthesis were done by the Opus session that owns the issue.

- **Part A (the engine)** was drafted by a Fable subagent: query model, durability, red/green,
  cycles, concurrency, cancellation, determinism, memory, persistence, optimization as queries,
  and the incremental == cold harness.
- **Part B (the C++ layer)** was drafted by a second Fable subagent: items and identities,
  program points, lookup, ADL, overload resolution, templates, constant evaluation, the
  preprocessor, granularity and soundness.
- **Review rounds.** The Opus session reviewed both drafts and sent six rounds of pushback or
  revision requests. Each changed the design:
  1. Validation originally short-circuited within a dependency group. That would make the set of
     cells verified at a revision depend on scheduling. Now every member of a group is verified;
     this costs nothing, because the re-execution would fetch the same group anyway (§A1.5).
  2. Diagnostics were collected transitively over dependencies. That would surface errors from
     speculative contexts such as SFINAE and concept checks, and it gave shared instantiations
     nowhere to keep their "required from here" chains. Now only errors that hold in every
     context are diagnostics, everything context-dependent is a value, and chains live on
     tagged edges (§A1.7).
  3. Part B's first draft keyed contextual queries by `(TU, position)` and kept a
     recency-ordered list of entries with a divergence hook. On review that raised three
     problems: results that depended on history, diagnostics that depended on garbage-collection
     timing, and a cell count that multiplied by the number of TUs. The Fable agent behind Part A
     reconciled the two models: contexts became content-addressed prefixes, traces became a trie
     per index, and divergence became the derived query `odr_report` (§A1.9). Part B was then
     updated to that contract by its author, who also corrected two of its own claims (§B6.2,
     §B6.6).
  4. DECISIONS §3.2–3.3 then made coroutines, `std::execution`, concepts, reflection, contracts
     and modules mandatory, with GCC 16.2 as the bootstrap compiler. The first draft of Part A had
     rejected senders inside the engine, so Part A's Fable agent redesigned concurrency,
     cancellation and the API on coroutines and senders, and probed GCC 16.2 (§A3, §A4, §A10).
  5. An independent review of the assembled document found four P0s, among them:
     - a tree id that ignored macro deltas;
     - TU-specific internal-linkage entity ids inside shared results;
     - "required from here" tags that were lost on trie hits;
     - no defined point of first demand.

     It also found fifteen P1s. Each part's Fable author fixed its own findings. Part B chose to
     carry demand lists in values, and Part A adopted them in place of edge tags.
  6. Part B's first fix for the point of first demand rejected some well-formed programs: those
     whose class template specialization would differ at a later site. On pushback it adopted the
     conforming rule, pinning the first instantiation (§B6.2).
- **Part C (the spike)**, §1 (decisions in brief), Part D (constraints on other designs) and
  Part E (open items and follow-ups) were written by the Opus session.

## 1. Decisions in brief

1. **Red/green queries with early cutoff by fingerprint.** A query is a pure function of its key.
   It reads the world only through recorded fetches. Its dependencies are stored as ordered
   *groups*, one per parallel fetch. Validation walks the groups in order, verifies every member
   of a group in parallel, and re-executes at the first group that changed. Cutoff compares
   128-bit Merkle fingerprints, so it still works when a value has been evicted or loaded from
   disk. Durability levels (system headers, third-party, project) make untouched parts of the
   table green in O(1). (§A1)
2. **Contextual queries.** A header's analysis, an instantiation or a constant evaluation is
   computed in a context: what precedes it in some TU. The context is a `Prefix`, an interned,
   content-addressed description of everything textually before the point. A demanded
   `(index, ctx)` is an ordinary red/green *selection cell*, where `ctx = (prefix, pinned,
   evaluating)`. Only the prefix is abstracted by the trie; the other two parts are ordinary key
   data and are empty in ordinary demands. When it must execute, it first
   walks a per-index trie of recorded traces (*verifying traces*), and reuses any result whose
   questions get the same answers at this prefix. At most one leaf can match, so selection never
   depends on history. Results are shared across TUs exactly where every question they asked
   agrees, not only where preambles are identical. (§A1.9, §B1.4)
3. **Identity without positions.** The preprocessed token stream is cut into *items* before
   parsing. An item's id comes from its file identity, its scope path, its declaration-part tokens
   and an ordinal among identical siblings, never from where it sits or from its body. Positions are
   `(item, offset)`. Two levels of identity:
   - Declaration ids are what lookup returns.
   - Structural entity ids key class definitions, layouts, IR, instantiations and ODR checks.
   (§B1.2, §B1.5, §B2)
4. **Every semantic observation is a question keyed by exactly what was asked**, with negative
   answers as ordinary values. The questions include:
   - visible declarations of one name in one scope at a prefix;
   - member lookup, with bases fetched only if the class itself has no member of that name;
   - using-directive closure;
   - ADL split into namespace members and hidden friends;
   - completeness before a point, and definition before a point;
   - one macro, and one include resolution, including headers that were not found.

   Adding a declaration changes only the answers that concern its name, in its scope, after its
   position. (§B2–§B5, §B7–§B9)
5. **Overload resolution** is memoized by the call's observable inputs: the exact candidate
   declaration set, argument shapes and kind of call. Identical call shapes walk to one
   trie leaf program-wide. A new overload re-analyses only bodies that looked up its name, and only bodies
   whose chosen candidate changed propagate further. (§B5)
6. **Templates.**
   - Function template definitions are instantiated in the end-of-TU context.
   - Class and variable templates, and concept satisfaction, are instantiated at the demanding
     site's own prefix. Specialization selection uses the instantiating computation's own prefix.
   - Each selection cell's value carries the list of instantiations it demanded, so `tu(TU)` can
     build, in source order, a table of first demands.
   - When a later site's class template specialization would differ from the first one,
     `tu(TU)` re-analyses that site with the first instantiation *pinned*. That reuses it, as
     [temp.point]/7 requires, and costs nothing for programs whose meaning is order-independent.

   The rules that a specialization must mean the same at every point, which are ill-formed with no
   diagnostic required (variable templates, satisfaction, specializations declared after a use),
   are *diagnosed* by comparing against END or the first demand. Divergence across TUs is
   reported by `odr_report`, a derived query over what each TU selected. (§B6, §B10)
7. **Cycles are internal errors, never recovered.** Every "in progress" situation in C++
   (completeness, deduced return types, mutually referential constant initializers) is a
   position comparison or an explicit key argument. Cycles are detected on a wait-for graph and
   reported deterministically. (§A2)
8. **Concurrency on coroutines and senders.**
   - Query executions are coroutines (`query_task`), and a cell's future is a `std::execution`
     sender (`cell_sender`).
   - The pool is a `std::execution` scheduler that also backs `parallel_scheduler`. It has two
     priorities, interactive and background.
   - A frame that has to wait suspends and frees its worker. Completions enqueue continuations
     rather than resuming them inline, so stacks never nest and no thread ever blocks.
   - `cheap` kinds, the question queries, are synchronous functions that may fetch only inputs and
     other cheap kinds. That keeps the hit path at about 100 ns with no allocation.
   (§A3)
9. **Cancellation uses stop tokens, and revisions advance only at quiescence.**
   - Each root request owns a stop source.
   - Once a stop is requested, `fetch` completes as stopped instead of suspending, and a stopped
     await destroys its frame without resuming it.
   - The daemon joins the roots' `counting_scope` before it writes inputs.
   - Every completed cell survives a cancelled request. Nothing half-computed is ever stored, and
     no exception is involved.
   (§A4)
10. **Determinism.**
    - Every order that reaches an output comes from stable keys.
    - Parallel results are positional.
    - Diagnostics are values with locations of the form `Loc = (item, offset)`. Only errors that
      hold in every context are diagnostics; everything context-dependent is part of a value.
    - `tu(TU)` renders and orders the diagnostics, and attaches "required from here" chains from
      demand lists. The driver merges diagnostics across TUs on the rendered location.
    - A banned list covers pointer order, unordered iteration, `std::hash`, thread ids and
      clocks.
    (§A5, §A1.7)
11. **Memory and persistence.** Values are kept in hash-consed interners. Eviction at quiescence
    has three levels: drop the value but keep its edges and fingerprint, drop unreachable cells,
    then compact. The on-disk cache is keyed by compiler build hash and schema hash and stores
    fingerprints, not handles. A restart runs ordinary red/green from a different starting state.
    (§A6, §A7)
12. **Optimization is a pure query** keyed by IR fingerprint, target, options, optimizer version
    and a budget level counted in deterministic units. A build's output is a function of three
    things: its inputs, the requested level, and an optimization-cache snapshot. The snapshot is
    an engine input, and can be exported for exact release replay. (§A8)
13. **Incremental == cold is tested, not hoped for.**
    - Edit generators drive the tests: random, mined from git history, and delta-minimized.
    - Each run compares outputs, plus the memo table via `ensure_green` with zero executions.
    - Runs repeat at several thread counts under a seeded chaos scheduler.
    - `--verify-memo` re-runs every green cell to catch missed dependencies.
    - Cancellation is injected at random points.

    The spike shows the approach working, including catching a deliberately missed dependency.
    (§A9, §B12, Part C)
14. **The engine is written in modern C++, as DECISIONS §3.2 requires.**
    - Concepts constrain query kinds, keys and values.
    - Reflection derives hashing, equality, fingerprints and cache serialization.
    - Annotations declare query traits.
    - Contracts state the engine's invariants.
    - Modules give the code layout.

    Probes compiled with GCC 16.2 (§A10.4) found no `// BOOTSTRAP:` workaround needed, with
    `std::execution` taken from libycxx. (§A10)

## Part A. The engine

*Drafted by a Fable subagent, revised after review (see Provenance).*

### A1. Query model

#### A1.1 Inputs and derived queries

An **input** is a value the daemon sets from outside: `file_text(path)`, `file_exists(path)`,
`dir_entries(path)` (include resolution reads directories, and a header that is *not* found is a
dependency like any other), `config` (the command line split into stable pieces: target, language
options, include paths, macro definitions), `build_time` (what `__DATE__`/`__TIME__` expand to;
pinned per build, never read from the clock), and `opt_cache_snapshot` (§A8). Inputs have no
dependencies and are the only things with `changed_at` set by hand.

A **derived query** is a pure function `run(Ctx&, Key) -> Value`. Everything it learns about the
world it learns through `cx.fetch`. There is no other channel: no globals, no ambient state, no
reading the file system. The API makes this physically hard (a query receives a `Ctx&` and its
key, nothing else) and review makes it a P0 defect.

Three flavours of derived query, chosen per kind by the dependency design:

- **Memoized** (the default): has an entry in the memo table, its own dependency list, its own
  value and fingerprint. This is what gives early cutoff.
- **Transparent**: not memoized. Its fetches are recorded into the *caller's* dependency list, as
  if the caller had made them. Use it for helpers that are called once per caller and would only
  bloat the table. A transparent query cannot act as a firewall; a memoized one can.
- **Contextual** (§A1.9): memoized, with a key `(index, ctx)`, whose execution first tries to
  select an already computed result whose recorded trace verifies at this prefix. This is how
  header analyses, instantiations and constant evaluations are shared across translation units.

#### A1.2 Keys

A key is `(kind, args)`. Kinds are enumerated at compile time (`QueryKind` is a small integer, and
the set of kinds, with their key and value types, is a versioned schema, §A7). Arguments are
values: integers, interned handles, small tuples of those. Requirements:

- **Equality and hashing by content**, with a fixed seed. Never a pointer.
- **A stable 128-bit fingerprint** derivable without the in-memory interner: every interned object
  carries a Merkle fingerprint computed from its content and its children's fingerprints at
  interning time. A key's fingerprint is the hash of its kind id and its arguments' fingerprints.
  This is what goes to disk (§A7); in memory, keys are compared by handle and hashed by handle hash.
- **Small**: a key should fit in 32 bytes. `visible_decls(S, name, prefix)` is three 32-bit handles. Large
  arguments (a token sequence, a macro environment) are interned first and passed by handle.

Negative results are ordinary values. `visible_decls` returning an empty list is an entry like any
other, and it is the entry that gets invalidated when a declaration with that name appears.

#### A1.3 Values

A value must be equality-comparable, fingerprintable, and serializable. In practice values are
either small plain data, or handles into interners (an AST node, an IR function, a token list). A
value is fingerprinted once, when it is stored. The fingerprint is `fp128(value)`, Merkle-style for
handles, so it does not depend on allocation order or on which process computed it.

Values are immutable once published. A query that wants to "modify" a value builds a new one.

#### A1.4 Revisions and durability

A **revision** `R` is a global counter advanced by the daemon when it applies a batch of input
changes (§A4.1). Every entry carries `changed_at` (the revision at which its value last actually
changed) and `verified_at` (the revision at which it was last known to be valid).

Each input has a **durability**: `High` (toolchain and system headers, resource files),
`Medium` (third-party dependencies under the project's control but rarely edited; the project
chooses the set, defaulting to everything under include paths marked `-isystem`), `Low` (the
project's own files, command line). The engine keeps `last_changed[d]` = the revision at which any
input of durability `d` last changed. A derived entry's durability is the minimum over its
dependencies. The shortcut: an entry with `verified_at >= last_changed[entry.durability]` is green
without touching its dependencies. After an edit to a user file, every entry whose transitive
inputs are all system headers (parsing `<vector>`, every instantiation entirely inside `std`) is
re-verified in one comparison. This is the single most important device for a daemon that holds a
whole program: without it, re-verification cost is proportional to the table, not to the edit.

Rejected: one revision counter per TU (shared header entries belong to no TU), and per-file
revision vectors (a dependency list would need a vector clock per entry; durability captures the
useful part of that at one byte per entry).

#### A1.5 Red–green validation with early cutoff

Dependencies are stored as an ordered list of groups, each group an ordered list of cell ids (§A3.6).
Program order, not completion order (§A3.7). The validation algorithm, per entry:

```
maybe_changed_after(cell, since, R) -> bool:        # "has this value changed since revision `since`?"
  e = table[cell]
  if e.verified_at == R:            return e.changed_at > since
  if e.is_input:                    return e.changed_at > since      # inputs are verified by the setter
  if e.verified_at >= last_changed[e.durability]:
      e.verified_at = R;            return e.changed_at > since
  claim(e, Validating) or wait for the claimant and retry
  for group in e.deps:                                   # in program order
      results = par_map(group, dep -> maybe_changed_after(dep, e.verified_at, R))   # every member, no short-circuit
      changed = any(results)
      if changed: break
  if not changed:
      e.verified_at = R;  release;  return e.changed_at > since
  # red: re-execute. Deps after the changed group may not be fetched at all by the new run.
  (value, fp, deps, diags, durability) = execute(e.key, R)        # claims Running
  if fp == e.fp:  e.verified_at = R                                 # early cutoff: backdate
  else:           e.value, e.fp = value, fp; e.changed_at = R; e.verified_at = R
  e.deps, e.diags, e.durability = deps, diags, durability
  release;  return e.changed_at > since
```

`fetch(key)` is `ensure_green(cell, R)` followed by reading the value, where `ensure_green` runs
the same procedure, and executes the query if the entry does not exist. Only the first changed
group is found before re-executing, because the old dependency list past that point describes a
run that no longer exists. Within a group, **every member is verified, with no short-circuit**,
even after one member is already known to have changed. This is free: all earlier groups are
unchanged, so the query's deterministic code, re-executed, reaches this same group with the same
key list and fetches every member anyway; verifying them now is the same work, done a little
earlier. It is also necessary: a short-circuit would make the set of cells verified at `R` depend
on which member happened to be found changed first, and §A5.5 and §A9 require that set to be a
function of inputs alone. The same reasoning shows why the walk must *not* continue into later
groups: those fetches may not exist in the new run, and verifying them could execute queries a cold
run never demands.

With this rule the set of cells a request leaves *valid* at `R` equals the set a cold run of the
same request demands, by induction over the dependency order: a cell verified green is one whose
re-execution would have been identical up to and including the group being verified, so every
cell it marks green is one the cold run fetches, and a cell re-executed is literally the cold run's
execution. One nuance: "valid" is not "has `verified_at == R`", because the durability shortcut
marks a cell green without visiting its dependencies. The harness (§A9) therefore compares the cold
run's table against the incremental table by asking `ensure_green` for each of the cold run's
keys, not by scanning `verified_at`.

Cutoff compares **fingerprints**, not values. Reasons: a value may have been evicted (§A6) while its
fingerprint stays, so cutoff still works and propagation still stops; the on-disk cache (§A7)
compares the same thing; and comparing two interned handles is comparing fingerprints anyway. A
128-bit collision among 2^32 values has probability about 2^-65; its only effect would be a missed
recomputation. We accept this, as every content-addressed system does, and state it in the
determinism contract.

Rejected: Adapton-style demanded computation graphs with change propagation from inputs
upwards. Upward propagation does work proportional to the dependents of an edit even when nothing
asks for them; a daemon serving an LSP wants the opposite: pay only for what is demanded, and let
durability make the undemanded parts free.

#### A1.6 Entry states

```
Empty ──claim──▶ Running(owner frame) ──store──▶ Done(verified_at = R)
Done(verified_at < R) ──claim──▶ Validating(owner) ──▶ Done(R)  or  ──▶ Running(owner) ──▶ Done(R)
Running / Validating ──abort (cancel, ICE)──▶ Empty / Done(old)     # never stores
```

The state word is one 64-bit atomic: 2 bits of state, 30 bits of owner frame id, 32 bits of
`verified_at`. Claims are a CAS. A frame that loses the claim waits (§A3.1). Publication is
release-store of the state word after the value is written; readers acquire-load the state, then
read the value. Because an entry's storage is never reused within a revision (§A3.6), there is no
ABA on the claim word.

#### A1.7 Diagnostics are values

A query reports a diagnostic with `cx.diag(...)`. The engine appends it to the executing frame's
list, and the list is stored with the entry as part of its output, fingerprinted with the value.
A diagnostic is a value: `(Loc, id, args)`, where `Loc = (item id, offset)` (§B2) is a function of
the item's own content, shared across TUs and prefixes. A `Loc` is neither renderable nor an
ordering key by itself: `tu(TU)` turns it into `(file, line, column, include chain)` from the TU's
ordered item sequence, and orders by item order in the TU, then offset (§A5.2). A diagnostic
contains no pointers, no text and no context chain; rendering, `-W` filtering, ordering and chains
are later, pure steps. Diagnostics are stored unfiltered, so changing warning flags changes the
report without invalidating a single cell.

**What may be a diagnostic.** `cx.diag` is only for a diagnostic that is correct in *every*
context that can reach the cell: the cell's key alone determines that the program is ill-formed
(or warrants the warning). Anything whose reporting depends on who asked is **part of the value**
and the consumer decides: substitution failure in the immediate context is a `Failed(reason)`
result of `substitute(...)`; constraint non-satisfaction is a value with its explanation attached;
"not a constant expression" is a value with its notes, and `static_assert` emits it while a
SFINAE-context use discards it; overload non-viability is a value. This is exactly the standard's
own shape ([temp.deduct] "immediate context"): a hard error met while instantiating a class
template on a SFINAE path *is* an error in every context, and C++ requires it to be reported, so
it is a `cx.diag`; a deduction failure is not an error anywhere by itself, so it is data. A cell
that is fetched only to answer a speculative question therefore has nothing in its own list that
could wrongly surface.

Rejected: a `fetch_speculative` whose edges `diags_of` does not traverse. It would suppress hard
errors C++ requires (the class-template case above), it makes a diagnostic's visibility depend on
the first path that reached the cell, and it duplicates in the engine a distinction the standard
already draws between values and errors.

**Collection.** `diags_of(cell)` = the entry's own diagnostics followed by, for each dependency in
program order, `diags_of(dep)`, memoized, pruned by a `has_diags` bit that is the OR over the
subgraph (the millions of entries with nothing to say cost one bit each), and **not descending
into `demanded` kinds** (below). `tu_diagnostics(TU)` is a separate kind (§A10.3): it fetches
`diags_of` of the TU's analysis cell (a cell fetching `diags_of` of itself would be a cycle,
§A2.1), adds the diagnostics of the cells the TU demanded with their chains, renders and orders
(§A5.2). A header's error appears in each TU that includes it, which is correct C++, and the
driver merges the copies.

**Context ("required from here").** Instantiation keys exclude the demanding TU (Part D), so the
chain of requests cannot live in the instantiation's own diagnostics. It does not live on
dependency edges either: an edge is not a value, so a tag on it would be absent from a trie hit
and from a restarted daemon, and the report would differ between a leaf that was computed and one
that was selected. It lives in **demand lists** (§B1.4). A kind whose cells are *demanded*
(`instantiate_*`, `satisfies`, `constant_value`; the `demanded` trait, §A10.1) is fetched with
`cx.demand<Q>(key, loc)`, never with `fetch`; the engine appends `(index, loc, selected fp)` to the
frame's demand list, which is part of the cell's value, fingerprinted with it and persisted with
the leaf. `tu(TU)` traverses the demand lists from its items in source order, builds its
first-demand table (§B6.2), and attaches each demanded cell's `diags_of` to the chain of request
positions leading to its first demand in that TU. The chain is a function of values alone: the
same whether the leaf was computed here or selected from the trie, and the same after a restart.

A consequence: an error inside `X<int>` requested from 500 sites in one TU is reported once, at
the first demand in that TU's source order, which is what a sequential compiler's point of
instantiation gives; the copies other TUs produce are merged by the driver (§A5.2). Fatal errors
are values too: a query that hits one returns a value saying "stopped here", and its consumers
decide what to skip; nothing throws across query boundaries (§A4.3, §A2.3).

Rejected: a global diagnostic sink. It makes diagnostics an effect, so an entry replayed from
memo would not re-emit them, and ordering would follow completion.

#### A1.8 Firewalls and projections

A projection (illustratively `decls_named(scope, name)` over `scope_members(scope)`; Part B's real
question is `visible_decls(S, name, prefix)`, §B2) needs no special status: it is a memoized query whose re-execution is cheap and whose fingerprint usually
does not change, so the propagation stops at it. The engine offers two hints, both about cost, not
semantics: `cheap` (a synchronous plain function, never a coroutine: it may fetch only inputs and
other `cheap` kinds, never waits, is computed again rather than awaited when another frame is
already computing it, and its value is never evicted, §A6.2; §A3.3) and `transparent` (§A1.1). The dependency design decides, per kind, which to use. The
engine's job is to make a memoized fetch cost about a hundred nanoseconds on a hit: hash two
handles, probe one shard, one acquire load, push one `u32` onto the frame's dependency list.

#### A1.9 Contextual queries

The C++ layer (Part B, §B1.4) needs one capability beyond the salsa model. A semantic
result (the analysis of a header item, an instantiation, a constant evaluation) is computed in a
*context*, what precedes it in some translation unit, but should be shared by every context in
which the computation would have observed the same answers. The device is the *verifying trace*
of Mokhov, Mitchell and Peyton Jones: store, with each result, the questions the computation
asked and the answers it got; reuse the result wherever the same questions get the same answers.
This section fixes how the engine provides it. It keeps the C++ layer's semantics and changes two
parts of its proposed contract: the context is a **prefix**, not `(TU, position)`, and the
per-index entries form a **trie**, not a recency-ordered list consulted by a hook.

**Contexts are prefixes.** A `Prefix` is an interned, content-addressed description of everything
that textually precedes a program point: for each level of the include path, the item list of
that region up to (not including) the include item that leads down, with include items represented
by the tree id of the tree they bring in; at the leaf level, the items of the item's own region up
to the item. A prefix node is `(parent prefix, item-list handle up to k)`, hash-consed, so its
digest is its fingerprint and two TUs that include the same headers in the same order up to a
point have the *same* prefix handle for that point, without anyone comparing anything. The END
context of a TU is the prefix of its whole tree. Positions inside an item are ordinary key data.
Every answer a semantic computation can ask of its context (visible declarations, macro state,
completeness before a point, specializations declared so far) is a function of the preceding
tokens, hence of the prefix; so a question cell is keyed by `(question, prefix)` and computes its
answer from the prefix's item lists and the region indexes, with no TU anywhere in its key.

Why this matters more than it looks: with `(TU, position)` in the key, every header item demanded
by 500 TUs would have 500 selection cells and 500 copies of each question cell. A TU that includes
the standard library has about 10^5 items; at five questions each that is 2.5×10^8 question cells
plus 5×10^7 selection cells across 500 TUs, 15–20 GB at the cell sizes of §A3.6, and 500 trace
verifications per item on every header edit. With prefixes, an item has as many selection cells as
there are distinct include orders leading to it, typically tens, so the same program is on the
order of 10^7 cells, under a gigabyte, and a header edit verifies each item once per distinct
prefix. "Context equivalence by prefix digest" is therefore not a verification shortcut
bolted onto TU-keyed cells; it *is* the key.

**Selection cells.** A contextual query is an ordinary query kind with `Key = (Index, Ctx)` where `Ctx = (prefix, pinned, evaluating)` (§B1.4; only the prefix is
abstracted, see below) and
the `contextual` trait. The cell for `(index, prefix)` is a **selection cell**: a red/green cell
like any other, whose value is the selected shared result (a handle) and its diagnostics, and
whose dependencies are the selected result's trace instantiated at this prefix. Early cutoff,
durability, cancellation, persistence and the determinism rules apply to it unchanged, because the
engine never sees anything but a cell with dependencies and a value. What is special is only how
it *executes*:

```
execute(sel(index, ctx)):                       # ctx = (prefix, pinned, evaluating)
  node = trie[index, ctx.pinned, ctx.evaluating].root   # one trie per index part; the prefix is what varies
  loop:
    if node is a leaf:                          # every question on the path answered as recorded
        record deps = path elements @prefix;  return (node.result, node.diags)
    answers = par_map(node.questions, q -> fp(fetch(q @prefix)))   # one trie node = one dep group
    edge = node.edges[answers]
    if edge missing:
        claim node for computing (others arriving here wait, then re-walk)
        (value, diags, trace) = run(index, ctx)               # ordinary tracked execution
        assert trace starts with the path to node             # determinism detector (debug: ICE)
        trie[index, ctx.pinned, ctx.evaluating].insert(trace with prefix abstracted, value, diags);  release
        return (value, diags)
    node = edge.target
```

The **trace** is the computation's complete dependency list in program order, groups included:
the question cells it asked at its prefix *and* its context-free dependencies, which have stable
ids: `body_tokens(item)`, `item_tokens(item)`, pure matchers, other selection cells. A trace never
names a content-hashed syntactic index (§B1.3); those are reached only inside question cells, and
what the trace records is the question's answer. Each element is `(key pattern, answer fingerprint)`, where a key pattern is
the dependency's key with the cell's own prefix replaced by a placeholder; context-free keys have no
placeholder. Including context-free dependencies is what makes the trace a *sufficient* set: a
body edit changes `body_tokens(item)`, the leaf stops verifying, and no stale result can be
selected by a context whose preamble did not change. Verification re-fetches every question at the
current revision through the ordinary red/green path, so a question cell that is stale is
validated or recomputed before its fingerprint is compared; there is no second validation scheme.

The selection cell records only the *selected* path as its dependencies. That is enough: the
result is a function of the answers along its path, because the computation is deterministic and
reads nothing except through recorded fetches; the answers to questions of other, rejected paths
cannot change which result is correct.

**The context has three parts; the trie abstracts one.** `Ctx = (prefix, pinned, evaluating)`
(§B1.4). `pinned` (a sorted map `spec → Ctx` naming the cell in which this TU first instantiated a
class template specialization, §B6.2) and `evaluating` (the constant-evaluation chain, §B7) are
ordinary key data: with the entity they form the *index part*, so there is one trie per
`(index, pinned, evaluating)`, and the one-leaf argument below applies to each unchanged. They are
empty in every ordinary demand, so in the common case the extra parts cost two zero handles per
key. Engine conditions for this to work: both maps are interned handles, so a key stays within 32
bytes (§A1.2); the prefixes *inside* a pin are not the frame's own prefix and sit inside an opaque
handle, so the abstraction rule (replace `[[=context]]` members equal to the frame's own prefix,
§A10.1) leaves them alone, which is right because a pin names a specific cell; a pinned fetch,
`instantiate_class(S, ctx.pinned[S])`, is therefore an ordinary stable-id dependency, verified by
fingerprint in the trace, kept alive by the dependency for garbage collection (§A6.2), persisted by
fingerprint (§A7.1), and walked by the wait-for graph like any other, so a pin that led back to the
pinning cell would be reported as a cycle rather than hang; the engine adds the contract
`pre(S ∉ ctx.pinned[S].pinned)` on `demand`. Cells with a non-empty `pinned` carry a TU-specific
context and are never shared across TUs, which is intended: they exist only to repair a divergence
`tu(TU)` found in that TU, under its source-ordered re-fetch loop (§B6.2).

**Why at most one leaf matches.** A deterministic computation asks the same first question group
in every context; each later group is a function of the answers so far. So the traces of one index
form a decision tree over answer fingerprints, a prefix follows exactly one path, and two leaves
cannot both verify in one context unless their paths are identical, in which case their results
are identical too. Recency and history are irrelevant to the result; the trie makes them
irrelevant to cost as well: verification is O(trace), not O(entries × trace), and a fresh run that
does not follow the existing path up to its divergence point has read something unrecorded or is
nondeterministic, which the `assert` above reports. The argument needs three things, all already
required elsewhere: completeness of traces (above); determinism and no untracked reads (§A1.1,
§A5.1); and results that mention nothing context-specific (declaration and entity ids, item-relative
positions, never a TU or a prefix; Part D). Its residual risks are the fingerprint collision of §A1.5
(the same 2^-65, with the same effect: a wrong reuse) and nothing else. Cancellation cannot leave a
partial leaf because a leaf is inserted by the ordinary store step, which never runs for an
unwound frame (§A4.3). A non-contextual dependency whose value changed between revisions is a trace
element like any other and fails verification.

Concurrency on the trie: nodes are appended under a per-index lock, which is taken only when a
path is extended (a miss), never on a successful walk. A frame that misses at a node claims it;
other frames arriving at the same node wait on that frame (an ordinary wait edge, §A2.2) and re-walk
when it finishes, finding either their edge or a later divergence. The waiting serialises only
contexts that agree on every answer so far and need fresh work at the same node, which is exactly
when they are likely to need the same result.

**Divergence is a query, not a hook.** Two leaves under one index with different results is the
IFNDR/ODR situation the C++ layer wants diagnosed. The trie must not be the thing that reports it:
after an edit history it holds leaves from contexts that no longer exist, a cold run would not have
them, and the report would then depend on when garbage collection ran. Instead
`odr_report(program)` is a derived query over the results **selected by live selection cells at the
current revision**: each `tu(TU)` already produces the list of entity-keyed indexes it demanded
with the result fingerprint selected for each (ordered by index); `odr_report` merges these lists
across TUs in command-line order, and an index with more than one distinct result is a diagnostic,
ordered by index and then by the stable keys of the TUs involved. The explanation ("`f(S, long)` is
visible at the end of `a.cpp` but not in `b.cpp`") is the first question group on one TU's selected
path whose answer differs when fetched at the other TU's prefix: computed from live cells by the
re-verification technique of §B6.6, not read from the trie. Within-TU
re-verification (point of instantiation versus END, first use versus END for specializations) is
two selection cells fetched by `tu(TU)` and compared; nothing new. The trie's shape is thereby a
pure cache: it can be emptied at any time without changing any output.

**Churn under edits.** An edit at item `k` of a region leaves the prefixes of items before `k`
unchanged, and with them the hash-consed item lists their question cells read, so nothing of
theirs re-executes; it gives new prefixes to items at or after `k` in that region and to
everything included after them in the TU. Those items get *new* selection cells, each executing as a trie walk: one
O(1) question fetch per trace element, shared by every TU with the same new prefix; the old cells
become garbage. The END prefix changes on any edit to the TU, so every END-context cell of that TU
re-walks; at ~10^4–10^5 instantiations per TU with ~10 questions each this is tens of milliseconds,
and it is inherent (an edit anywhere in the TU can add an overload visible at END). Cutoff
happens at the level of values, not keys: `tu(TU)` re-executes because its dependency keys changed,
fetches the new cells, finds the same shared results, and its consumers (per-entity code
generation keyed by IR fingerprint) are green.

**Garbage collection and persistence.** A trie leaf is live if some live selection cell (§A6.2
step 2) selects it; nodes with no live leaf below them are pruned at quiescence, after the same
grace of `k` revisions that values get, so that undoing an edit finds its old leaf. A leaf's value
may be evicted with its fingerprint kept (§A6.2 step 1); a selection cell that then needs the value
re-runs the computation at its prefix and asserts the fingerprint. On disk a leaf is one record
`(index fp, path as (pattern fp, answer fp) groups, result fp, result bytes, diags)`; pattern
fingerprints are stable because placeholders and item ids are. Question cells are **not**
persisted: they are O(1) to recompute, and a loaded selection cell is stored with no dependencies
and marked red, so its first fetch is a trie walk that recreates exactly the question cells it
needs. This settles the question, left open by Part B's first draft, of whether question cells are persisted.

**Verification and the harness.** `--verify-memo` (§A9) treats a selection cell as any other cell,
with two additions: the fresh run bypasses the trie, and besides comparing the result fingerprint it
compares the fresh trace with the stored path element by element, so a missed dependency and a
nondeterministic question order are both reported with the first differing element. The
incremental == cold table comparison is unchanged, since selection cells are cells: `ensure_green`
on each cold key with zero executions and equal fingerprints. One check is added in debug runs: the
set of trie leaves selected by live cells must be identical between the two runs (the cold trie
contains nothing else, so this is the cold trie compared with the live part of the incremental one).

**Rejected.** `(TU, position)` as the context (cell count multiplies by the TU count; verification
sharing becomes a separate mechanism that has to be proven equivalent to the keys it bypasses).
A recency-ordered entry list with a divergence hook (O(entries × trace) verification; the hook is
an effect whose output depends on GC timing and edit history). Context in the key (the C++ layer's
own rejection: the context is unknowable before the computation runs). Traces of contextual
questions only, with context-free dependencies validated separately (a stale leaf would verify
after a body edit). Persisting question cells (large, regenerable in O(1)). Making question
queries transparent (their fingerprints are what the trie branches on, so they must be cells).

### A2. Cycles

#### A2.1 Cycles are internal errors, never recovered

A cycle in the demand graph means the compiler asked a question whose answer depends on itself.
The engine treats this as an internal compiler error: no fixpoint, no fallback value, no
"provisional result". Reasons, in order of weight:

1. **Determinism.** A cycle's "recovery value" depends on where the cycle was entered, and the
   entry point depends on which thread or which TU demanded a member first. Every scheme we
   examined that assigns values to cycle participants (poison the detected cycle, poison the
   strongly connected component, iterate to a fixpoint) leaks the entry point into some value
   unless the engine re-runs the whole component sequentially, which neither bounds cost nor
   composes with memoization across TUs.
2. **C++ does not need it.** Every "cyclic" situation in the language is defined positionally or
   via an in-progress state that can be made an explicit key argument (§A2.4). The front end can
   always ask a question that has an answer.
3. **Fixpoints that are real** (interprocedural analysis over recursive call graphs in the
   optimizer) are computed by a query whose key is the strongly connected component, and whose
   body iterates internally. That is deterministic and memoizable. It is a constraint on the
   optimization design (Part D).

So the engine's cycle machinery exists for two reasons only: a cycle must not deadlock the daemon,
and it must be reported in a way that lets someone fix the compiler.

#### A2.2 Detection across threads

Every executing query is a **frame**: the coroutine frame of its `query_task` (§A3.1), whose
promise holds `{cell, parent frame, root request, path index in parent's group, waiting_on}`.
A frame is never a thread; it is suspended whenever it waits. It can wait on a cell in three ways:
a fetch of a cell that another frame is executing or validating; a child execution it started by
symmetric transfer (the parent waits on the child's cell until the child completes); and a
parallel group whose members are still running (waiting on each incomplete member's cell).
Together with each cell's owner frame, this is a wait-for graph.

When frame `F` is about to suspend on cell `c` owned by frame `G`, it takes the engine's single
**wait mutex**, records `F.waiting_on = c`, and walks: `G`, then whatever `G` waits on, and so on.
If the walk reaches `F`, there is a cycle; otherwise `F` suspends. The mutex is taken only when a
frame suspends on a foreign cell or is resumed from one, which is rare relative to fetches (it
happens when two frames want the same not-yet-computed entry at the same time). Child and group
edges are written by the owning frame with release stores before the child starts; they are
consistent for the walk because a cell's owner does not change while it is `Running` within a
revision, and a finished cell has no owner, which ends the walk. A frame waiting for another frame's claim on a trie node (§A1.9) is an edge of the same
kind, with the node's claimant as owner, so node claims and cell claims are walked alike. Since
frames are not threads, the walk is the same whether the frames involved sit on one worker or on
twenty.

#### A2.3 Deterministic reporting

On detection, the detecting frame computes the set of cells on the cycle and sorts their keys by
`(kind id, key fingerprint)`. The ICE is `"internal error: query cycle"` with that sorted list,
and, as a note, the program-order path from the detecting frame's root. Every frame on the cycle
is completed through its `set_error` channel with the cycle report, which destroys its frame
(nothing is stored) and resets its cell to `Empty`; the error propagates through each awaiting
parent the same way to the root request, which fails. Because the sorted set depends only on the cycle and
not on which member was entered or which thread found it, the same cycle always prints the same
message. When a strongly connected component contains several cycles, parallel runs may find
different ones; since this is an ICE in any case, the canonical report is defined as the one a
`--threads=1` run prints (program-order DFS), and the ICE message says so.

#### A2.4 Which C++ situations look like cycles, and how the front end avoids them

The rule for the dependency design: **anything a sequential compiler tracks as an "in progress"
flag is either a position comparison or an explicit key argument.**

- **Class completeness.** `struct S { static constexpr int n = sizeof(S); };` is ill-formed because
  `S` is incomplete at that point; it is not a cycle. Completeness is positional:
  `complete_before(S, prefix)` (§B8) is a question cell, true at prefixes after the closing brace
  and in complete-class contexts (member function bodies, default arguments, default member
  initializers, noexcept-specifiers). Analysing a member declaration must never demand
  `layout(S)` or `class_def(S)` for the enclosing class; it asks `complete_before` at its own
  prefix. An instantiated class asks the same question at the instantiation's prefix.
- **Deduced return types.** [dcl.spec.auto.general] is positional: a function with a placeholder
  return type cannot be used before its definition has been seen, and within its own body a
  return type deduced from an earlier return statement is usable. So `return_type(f)` at a use
  site compares positions first and demands the body only when the definition precedes the use.
  Mutual recursion among templates (`f<T>` calls `g<T>` calls `f<T>`, both `auto`) is where a
  sequential compiler consults a "being deduced" flag. We make the flag a key argument:
  `return_type(f<int>, deducing = {})` instantiates the body with `deducing = {f<int>}`; the call
  to `g<int>` asks `return_type(g<int>, {f<int>})`; its body's call to `f<int>` finds `f<int>` in
  the context and yields the error type with the diagnostic. No cycle, and every key has exactly
  one value, so the result does not depend on whether `f` or `g` was demanded first. The cost is
  that `g<int>`'s body may be instantiated twice (once under `{f<int>}`, once under `{}`); this
  only happens in programs that are ill-formed or nearly so.
- **Constant evaluation of variables whose initializers refer to each other**
  (`template<class T> struct X { static constexpr int n = X<T>::m; static constexpr int m = X<T>::n; };`).
  Same device: `constant_value(var, prefix, evaluating)` (§B7), where `evaluating` is the ordered
  set of variables whose initializers are being evaluated on this chain, threaded likewise through
  `instantiate_fn/var(spec, prefix, evaluating)` for the specializations an evaluation needs. A
  variable found in `evaluating` is "not usable in constant expressions" ([expr.const]) without a
  fetch. The set is empty in all ordinary evaluations, so the key space does not blow up.
- **Default member initializers used in the class.** DMIs are complete-class contexts, so they are
  evaluated with the class complete; a DMI that uses another DMI uses it through a member
  access on a complete class. Not a cycle. Order-of-initialization questions are positional.
- **Recursive template instantiation** (`X<N>` needs `X<N-1>`) is depth, not a cycle; the depth
  limit is checked by the instantiation query itself. `X<N>` needing `X<N>` through a member
  initializer is caught by the evaluation context above.
- **Constexpr recursion** runs inside one `constant_value` (or one evaluation of a
  constant-expression context) with a step limit; individual calls are not queries, so infinite
  recursion is a step-limit error, not a cycle.

If a corpus program produces an engine cycle, the fix is in the front end, found by the
`--threads=1` report; the test suite (§A9) treats an ICE as a failure like any other.

### A3. Concurrency

#### A3.1 Shape: executions are coroutines, cell futures are senders, the pool is a scheduler

A query execution is a coroutine of type `query_task<Value>` (ycxx's own promise type, §A10). It
suspends when it needs something that is not yet available: a cell another frame is executing or
validating, or the completion of a parallel group. A suspended execution holds no thread. Its
frame is linked into the cell's waiter list and the worker returns to its deque. Each worker is a
trampoline, `while (t = pop() or steal()) t.resume()`, and every resumption begins at that loop's
depth, so there is no stack nesting anywhere in the engine. This removes the three devices an
earlier draft needed because frames were thread stacks: compensation threads, the rule that a
joiner may only pop its own join's tasks, and the false-deadlock hazard of helping.

Three `std::execution` entities make this concrete:

- **`pool_scheduler`** satisfies `std::execution::scheduler`. `schedule()` returns a sender whose
  operation state is a deque task: pushed onto the calling worker's deque, or onto the injection
  queue when `start` runs on a non-worker thread (the file watcher, the LSP transport). The pool
  also implements `std::execution::parallel_scheduler_replacement::parallel_scheduler_backend`
  and is installed at daemon start, so `std::execution::parallel_scheduler` anywhere in ycxx *is*
  this pool: one set of worker threads per process. Query code never names a scheduler.
- **`cell_sender(cell, priority)`** is the future of a memo cell: a sender that completes with
  `const Value&`, on the pool (`get_completion_scheduler<set_value_t>` is `pool_scheduler`).
  `connect` yields an intrusive operation state that lives in the awaiting frame, so waiting
  allocates nothing. `start` is `ensure_green`: a `Done` cell completes at once; a `Running` or
  `Validating` cell links the operation state into the cell's waiter list; an `Empty` cell is
  claimed and its execution started. On completion the owning frame publishes `Done` and
  **enqueues** every waiter's continuation onto its own deque at the waiter's priority; it never
  resumes a waiter inline from the completion path. The one exception is a child started by
  symmetric transfer with its parent as sole waiter, which transfers back to the parent the same
  way, which costs nothing and nests nothing.
- **`query_task<V>`** is a sender as well as an awaitable (`as_awaitable` finds its `connect`), so
  it composes with `std::execution::task`, `when_all`, `counting_scope` and `spawn` in the daemon
  code that calls into the engine. Its `set_stopped` channel is cancellation (§A4) and its
  `set_error` channel is an internal error: a cycle (§A2.3) or an exception escaping query code.

Why ycxx's own promise rather than `std::execution::task` for every execution: `task`'s
environment carries a type-erased `task_scheduler`, with a virtual call per schedule and (in
libycxx today) a backend allocation per construction; its scheduler affinity re-schedules through
`schedule()` after an await completes elsewhere, whereas the engine wants the *completer* to
enqueue continuations in deque order, which affinity cannot express; its frames are allocated
through the `allocator_arg` protocol rather than the per-worker free lists of §A3.3; and it knows
nothing of dependency recording. `std::execution::task` is used where it fits: the daemon's request
handling, the LSP and watcher transports, the persistence writer, the test harness. Both are
senders, so they compose freely.

#### A3.2 The scheduler

The pool has `P` workers (hardware threads by default). Each worker owns one Chase–Lev deque per
priority level (`interactive`, `background`). A deque task is an intrusive node with a resume
function: a coroutine handle for query executions and enqueued continuations, an operation state
for `schedule()`. A worker pushes to and pops from the top of its own deques, interactive first;
an idle worker steals from the bottom (oldest task) of a random victim, trying every worker's
`interactive` deque before any `background` one, spins briefly, then parks on a futex guarded by
a global pending-task count. Nothing ever blocks a worker except an empty system.

`par_fetch(k1..kn)` records its group (§A3.4), then starts members `n..2` as deque tasks and member
`1` by symmetric transfer, leaving the parent suspended with a counter of incomplete members.
Each completing member decrements it; the last enqueues the parent. Thieves take the earliest
members first (they steal from the bottom), the owner works through the later ones, and none of
this is visible in values because the dependency order was fixed before any member ran.
`par_fetch` has `when_all`'s completion contract (values positionally, stop or error on the first
member that stops or errors, with stop requested on the rest) and is implemented in the engine
rather than as `when_all(cell_sender...) | continues_on(pool)` only for the fast path of §A3.3:
when every member is already `Done`, no sender is connected at all.

#### A3.3 The fast path and the cost of an execution

`co_await cx.fetch<Q>(k)` is an awaitable whose `await_ready()` does the whole hit path: hash the
key, probe the shard, acquire-load the state word, and if the cell is `Done` and verified at this
revision, push one `u32` onto the frame's recorder and return `true`. No frame is created, no
sender connected, nothing allocated: about a hundred nanoseconds, and that is the path millions of
fetches per build take. Only `await_suspend` runs `ensure_green`: it claims the cell for validation
or execution, allocates the child's coroutine frame, and starts it by symmetric transfer.

Coroutine frames come from per-worker size-class free lists. A frame freed on another worker (its
execution migrated after a suspension) goes onto that worker's list; frames are fungible memory,
so no worker owns a frame. GCC 16 does not elide frames (HALO; probed: a child coroutine awaited
by its parent still allocates), so the budget assumes one allocation per executed query: a
free-list pop and push, about twenty nanoseconds, plus promise construction and the final
suspension, under a hundred nanoseconds in all, and a frame of roughly 128–512 bytes whose size is
dominated by the query's own locals. Against the microseconds a non-trivial query costs to run,
this is noise; it is not noise for the millions of tiny lookups, which is why those are not
coroutines:

A **`cheap`** kind is a plain synchronous function, not a coroutine. Its `run` takes a `SyncCtx`
whose `fetch` is constrained to inputs and other `cheap` kinds, so it can never need to wait, and
the layering is enforced at compile time by concepts. If it finds its cell `Running` under another
frame (two frames computing the same tiny lookup at once), it computes the value again instead of
waiting; determinism makes the two values equal, and the second publish verifies that with a
`contract_assert` on the fingerprint and discards its copy. The C++ layer's question queries
(`visible_decls`, `macro`, the region indexes) are `cheap`. A `transparent` helper is a coroutine
when it fetches non-cheap kinds and a plain function otherwise.

#### A3.4 Deadlock-freedom with suspended frames

Every suspended frame waits on cells, and every `Running`/`Validating` cell has an owner frame. An
owner frame is either runnable (in a deque, or being resumed) or itself suspended. Suppose no
frame is runnable while some frame is suspended. Following owner edges from any suspended frame
then stays inside the finite set of suspended frames and revisits one: a cycle, which the wait-for
walk (§A2.2) detects at suspension time and breaks by completing the participants with an error.
So whenever a frame is suspended, some frame is runnable, and since the scheduler eventually runs
every enqueued task (idle workers steal; the pending count wakes a parked worker on every push to
an empty system) and every query terminates, it makes progress. Tasks pushed but not yet claimed
delay nobody: they are runnable by definition. Priorities, the number of workers and the order
of stealing are irrelevant to the argument; they decide only how many cores do the work.

**Rejected alternatives.**
- *Fibers (stackful coroutines per frame).* Stackless coroutines give the same freedom without a
  hand-written context switch per target, a stack per in-flight frame, or sanitizer and debugger
  trouble. The only cost fibers would avoid is turning `run` into a coroutine, which the charter
  wants anyway.
- *Threads as frames, blocking with compensation (the previous draft).* Correct but needs spare
  threads, a restricted join rule and a stack-nesting argument; coroutines remove all three.
- *`std::execution::task` for every execution.* See §A3.1: type-erased scheduler, re-scheduling
  affinity, allocator protocol, no dependency recording.
- *Resuming waiters inline on the completing frame's stack.* Fast, but a chain of completions
  (child completes, resumes parent, which completes, resumes grandparent, …) nests to the depth of
  the demand graph, which for instantiation chains is thousands. Enqueueing costs a push and a pop
  and keeps the stack flat.
- *Making `cheap` kinds coroutines too.* Uniform, and a frame allocation per `visible_decls` call
  would dominate the engine's cost.

#### A3.5 Interactive versus background work

A root request carries a priority. LSP requests are `interactive`; builds and the optimizer's
idle work are `background`. A continuation is enqueued at its root's priority, workers prefer
`interactive` deques when stealing, and a worker that finishes a `background` segment checks for
`interactive` work before popping its own `background` deque; this is safe at any point, because no
worker ever joins on its stack (§A3.1), so there is no join during which stealing could nest. There
is no preemption inside a
segment (the code between two suspension points), but a segment is at most the work between two
fetches that miss, so interactive latency is bounded by that, not by a whole task. An
`interactive` frame that needs a cell being computed by a `background` frame suspends on it; the
background frame continues at full speed on whatever worker runs it. Long-running leaves (solvers,
§A8) poll `cx.stop_token()` at their deterministic budget checkpoints and may `co_await
schedule(pool)` there to yield the worker; yielding changes no value.

#### A3.6 Shared memory layout and lifetime

The memo table is a sharded concurrent hash map from `(kind, key)` to a **cell id**: a `u32`
index into a per-kind entry arena. Cells are never freed or reused while any frame is running;
garbage collection happens only at quiescence (§A6.2). Consequently readers never hold a cell that
can disappear, and dependency lists can be plain `u32` vectors. A cell entry is:

```
state      : atomic<u64>      # state | owner | verified_at   (§A1.6)
changed_at : u32
durability : u8,  has_diags : u1,  flags (cheap, value_evicted) : u6
fp         : u128             # fingerprint of (value, diags)
value      : index into the kind's value store (or evicted)
deps       : offset+len into a per-kind dependency pool; groups delimited by a high bit
diags      : offset+len into the diagnostic pool (usually empty)
```

About 48 bytes plus 4 bytes per dependency. Ten million cells with five dependencies each is
under a gigabyte, which is the order of magnitude a whole-program daemon must afford.

Memory ordering: the state word is the only synchronization point. A writer fills value, deps and
diagnostics, then release-stores `Done`. A reader acquire-loads `Done` and may then read all of
them without further synchronization. Waiters are the intrusive operation states of `cell_sender`
(§A3.1), linked from one `waiters` pointer per cell by CAS and taken by the completing frame, so
cells carry no condition variable and a wait allocates nothing.

#### A3.7 Deterministic dependency recording

Each frame has a dependency recorder: a vector of cell ids with group markers and a small
"already recorded" set. `fetch` appends one singleton group; `par_fetch(k1..kn)` appends one group
with members in **argument order** before any member runs, so completion order never shows. A key
fetched twice by the same frame is recorded once, at its first position (validation of the first
occurrence covers the later ones). Child frames record into their own lists; a transparent query
records into its caller's. On store, the recorder is compacted into the dependency pool.

Validation of a group in parallel (§A1.5) is itself a `par_fetch` of `ensure_green` tasks, so the
same scheduler serves both.

A frame may resume on a different worker after any `co_await`. Nothing thread-local is therefore
cached across a suspension point: the recorder and the scratch arena belong to the frame
(§A6.1), the worker is re-read from the resuming context, and `thread_local` is banned in query
code (§A5.1). The chaos scheduler (§A9) migrates frames deliberately to expose violations.

### A4. Cancellation

#### A4.1 Revisions advance only at quiescence

The daemon coalesces file-system events into a batch. Every root request owns an
`std::inplace_stop_source`; its token is the `get_stop_token` of every frame under that root and
is what `cx.stop_token()` returns. To apply a batch the daemon requests stop on every live root,
then `co_await`s the join of the `counting_scope` that holds the roots, then writes the inputs and
advances `R`. Cancellation is cooperative and exception-free: the `fetch` awaitable tests
`stop_requested()` on entry and completes with `set_stopped` instead of suspending; a frame
already suspended on a cell has an `inplace_stop_callback` registered by its operation state,
which unlinks it from the waiter list and completes it stopped. A `query_task` whose awaited
sender completes stopped does not resume the coroutine: the promise's `unhandled_stopped` destroys
the frame (destructors run, the cell it was executing reverts to `Empty` or to its old `Done`) and
completes the parent's await stopped, and so on up to the root's receiver. Latency to quiescence is
bounded by the longest segment between two fetches or polls, which is why solvers and the code
generator poll the token at their budget checkpoints.

Why not let the old revision keep computing (snapshot isolation)? It would need every cell to hold
two versions, or a copy-on-write table, for the duration of in-flight work, and the work being
finished is work on stale inputs that the next request will redo anyway. Why not cancel without
waiting for quiescence? A frame that read dependency `a` before the bump and `b` after it would
compute a value from two worlds; the only way to prevent that without stopping the world is to
version every read. Stopping the world is cheap because cancellation is cooperative at every
fetch.

#### A4.2 What survives

Everything that reached `Done` survives: those cells hold values that are correct for the
revision they were verified at, computed entirely from dependencies verified at that revision,
and the next revision validates them like any other cell. This is the "partial reuse of completed
sub-results": a cancelled build of 400 TUs keeps every function body, header parse and
instantiation it finished. Nothing in progress survives; the half-finished frame's partial state
is unwound by RAII, and its cell was never written.

#### A4.3 Cancellation cannot leak into results

The invariant: **a cell is stored only by a frame whose every fetch happened at the frame's start
revision.** It holds because the revision cannot change while any frame runs (§A4.1): either the
frame finishes before the bump and its value is correct for that revision, or the bump is pending,
the flag is set, and the frame's next fetch throws before it can store. A frame with no further
fetches that completes while the flag is set stores a value that is still correct for the old
revision, because nothing has changed yet. The store happens only in `co_return`, which is the
`set_value` path; the `set_stopped` and `set_error` paths never reach it, and query code cannot
observe either (a stopped await simply never resumes the coroutine), so there is no exception to
catch and nothing to get wrong. An exception thrown by query code reaches the promise's
`unhandled_exception`, completes the frame with `set_error`, and is an internal compiler error; a
`catch (...)` in query code is a review P0 because it would hide one.

Determinism is untouched because the memo table never affects *what* a value is, only whether it
is recomputed. A cancelled run leaves a table that is a subset of what a complete run leaves, with
every stored value equal to the from-scratch value at its revision.

#### A4.4 LSP latency

A completion request arriving with an edit: the build in progress is cancelled (its results were
about to be stale), the revision advances, the completion runs at `interactive` priority on
largely green state (the edit touched one file; durability keeps the rest green), and the build
restarts behind it at `background`. A request arriving without an edit runs concurrently with the
build at higher priority. In both cases the request's cost is the red part of its own demand
graph, not the build's.

### A5. Determinism

#### A5.1 Rules

1. Every order that reaches an output is derived from stable keys: source order as `tu(TU)`
   renders it from its item sequence and `Loc` offsets (§A5.2; a `Loc` alone is not an ordering
   key), mangled names, the command-line order of TUs. Never from completion order, cell ids
   (allocation order differs between runs), thread ids, addresses, or time.
2. Parallel operations return results positionally: `par_fetch` returns a tuple in argument
   order; `par_map(keys, f)` returns a vector in key order. A parallel reduction is a `par_map`
   followed by a sequential fold over that vector, or a tree reduction whose shape depends only
   on the key count. Associative-but-not-commutative folds are therefore safe; floating-point
   folds are safe because the tree shape is fixed.
3. Banned in query code, enforced by review and by a lint over the engine's allowed-API list:
   pointer hashing and pointer comparison for ordering; iteration over unordered containers
   except to build a sorted result; `std::thread::id`; the clock; `std::hash` with its
   implementation-defined seed (ycxx uses its own fixed-seed hash everywhere); sorting by anything
   address-derived; non-reproducible RNG (solvers get a seed derived from the key fingerprint).
4. The engine's own randomness (steal victim selection) never reaches values.

#### A5.2 Diagnostics

`tu_diagnostics(TU)` renders and orders (§B2): each `Loc = (item id, offset)` becomes
`(file, line, column, include chain)` from the TU's ordered item sequence, and the TU's order is
that sequence, then the offset. Chains ("in instantiation of … requested here") come from the TU's
first-demand table (§B6.2, §A1.7): the diagnostics of a demanded cell are attached, once, to the
chain of request positions leading to its first demand in that TU, so a shared instantiation's
error is reported once per TU, at the point of instantiation a sequential compiler would name.
The build report concatenates the TUs in command-line order and the driver deduplicates on the
rendered `(file, line, column, severity, id, args)`, keeping the first TU's chain; the report's
order is file, line, column, severity, id, args, then chain. A header error seen by 500 TUs is one
line. Per-file LSP diagnostics are the per-TU result filtered by file. Error limits are applied
after ordering, so "the first 20 errors" means the first in source order, not in time.

#### A5.3 Symbols and sections

Object emission orders functions and variables by their definition's stable position in the TU
(with instantiations after, ordered by their mangled name), sections by a fixed table, string
tables by first use in that order. The optimization design may choose a code-layout order, but it
must be a function of the IR and the cost model, both values. Linking orders by input order and
then by the same per-object order.

#### A5.4 First-error-wins

A fatal error (a missing header, a `#error`, an exceeded limit) is a value returned by the query
that hit it. The preprocessor and parser of one TU are sequential in file order, so "everything
after the fatal point is skipped" is well defined. Across TUs, nothing is "first": each TU reports
its own result and the report sorts them. Where the standard or usability wants one diagnostic
for a situation that several parallel queries can observe (an ambiguous call diagnosed at several
instantiation sites), each query emits its own and deduplication merges them.

#### A5.5 Checking it

Two runs with different thread counts and a chaos scheduler (seeded random steal order, random
deque reversal, random short delays at fetch) must produce byte-identical objects and reports.
Beyond outputs, the memo tables' `(key fingerprint, value fingerprint)` sets must be identical:
every value, not just those that reach outputs, is a pure function of inputs. §A9 has the harness.

### A6. Memory

#### A6.1 Interning and arenas

Large values live in interners, one per type (tokens, token sequences, AST nodes, types, IR
functions, diagnostics). Interning hash-conses by content and assigns a dense handle and a Merkle
fingerprint. Values in the memo table are handles, so a header parsed once is one set of nodes
shared by every TU that includes it, and two TUs instantiating `std::vector<int>` share the
instantiation. Interners are append-only during a revision and compacted at quiescence.

A frame owns a bump allocator for scratch (a chunk list hanging off its promise, so it follows the
frame when it resumes on another worker) that is freed when the frame completes; anything that
outlives the frame must be interned. This is the memory discipline that keeps millions of small
queries from fragmenting the heap.

#### A6.2 Eviction policy

Every cell records `last_used` (the revision at which it was last fetched). Memory pressure is
handled at quiescence in three escalating steps:

1. **Drop values, keep edges and fingerprints** for cells not used in the last `k` revisions. The
   cell can still be validated green (its dependencies and fingerprint are intact) and still cuts
   off propagation; if its value is needed, it is recomputed and compared against the fingerprint
   (a built-in missed-dependency check, §A9). `cheap` cells are exempt: their value is smaller than
   the recomputation.
2. **Drop cells** not reachable from any root demanded in the last `K` revisions, including their
   edges. Reachability is computed over the dependency graph from the roots the daemon still
   serves (open LSP documents, the last build).
3. **Compact interners**: mark from live cells' values, sweep, renumber handles (a renumbering
   table is applied to the memo table; fingerprints do not change). Cells are renumbered at the
   same time, and dependency lists rewritten.

The budget is a configured byte count (default: a fraction of physical memory); the daemon reports
its usage through the control interface. A whole program's analysis fits: it is the same
information a sequential compiler holds for one TU at a time, deduplicated across TUs.

### A7. Persistence across restarts

#### A7.1 Format

The cache directory is keyed by the **compiler build hash** (a hash of the ycxx binary) and the
**schema hash** (kind ids, key and value layouts, per-kind version salts). A mismatch means an empty
cache; there is no migration. Inside, an append-only log of segments plus a manifest written by
atomic rename. Each record is `(kind, key bytes, key fp, value fp, deps as (kind, key fp) list
with group markers, durability, value bytes or absent, diag bytes)`, checksummed. Key bytes are
the structural serialization of §A10.1 with interned handles written as the fingerprint of the
interned content, so a loaded dependency that turns red can be re-executed from its record; a
record whose key names an interned object the cache does not contain is dropped. Inputs are
recorded with their content fingerprint. Trie leaves of contextual queries are a second record
type (`index fp, index bytes, path of (pattern fp, answer fp) groups, result fp, result bytes
including the demand list, diags`), and selection cells are stored without dependencies (§A1.9).
Revisions are not persisted; they are a session concept.

Fingerprints, not handles, are what records refer to each other by, so the file does not depend
on interning order, two daemons' caches can be merged, and the optimization cache (§A8) can be
shared between machines.

#### A7.2 Loading

On start the session's revision counter is `R = 1`. Records are loaded with `verified_at = 0`
and `changed_at = 0`; the interner is rebuilt by deserializing values (fingerprint to handle map).
The daemon hashes every input file it knows about (in parallel; this is the cold cost of a
restart): an input whose fingerprint matches keeps `changed_at = 0`, one that differs gets
`changed_at = 1`, and `last_changed[d]` is `1` for every durability with a changed input and `0`
otherwise. The first request then runs the ordinary red–green walk: a loaded cell of a durability
with no changed input is green by the shortcut (`verified_at 0 >= last_changed 0`), everything
else by deep verification, re-executing from the persisted key bytes where a dependency turned
red. There is no separate "cache validation" algorithm; it is the same algorithm with a different
starting state.

#### A7.3 Crash safety and trust

Segments are written by a background writer at quiescence and on shutdown. A torn tail is
truncated at the last valid checksum; a corrupt record is dropped. The cache is never trusted for
correctness beyond what red–green guarantees for in-memory entries: a loaded derived entry is
only ever *used* after its dependencies verified green at this session, and a loaded value whose
fingerprint does not match its bytes is dropped. Debug builds can additionally sample loaded
entries and recompute them (§A9).

### A8. Optimization as queries

#### A8.1 The pure core

`optimized(ir_fp, target, options, optimizer_version, budget)` is an ordinary memoized query. Its
budget is a level on a ladder (`0, 1, 2, …`, each a fixed vector of deterministic units: e-graph
iterations and node limits, solver conflict limits, superoptimizer candidate counts), never time.
Given the same key it produces the same value on any machine and any thread count, because the
search is deterministic, parallel solvers run in their deterministic modes, and all ordering comes
from the IR. Its value includes the verification evidence (translation-validation certificate
fingerprint or "proven rule set"), and on verification failure or budget exhaustion it is the
heuristic result for that region. Nothing else in the compiler talks to the optimizer.

#### A8.2 The anytime layer

While idle, the daemon runs `optimized(k, budget = next level)` for functions picked from a work
list (hot by profile, recently changed, largest measured gap to the oracles), each as a
`background` root. Results go into the **optimization cache** (OC): a persistent table from
`(ir_fp, target, options, optimizer_version)` to the best known `(budget level, result fp)`,
"best" meaning highest budget level, with the cost model's estimate recorded for reporting.

#### A8.3 What a build's output is a function of

Precisely: **inputs, the requested budget level `B`, and the OC snapshot `S` taken when the build's
revision was created.** `S` is an *input* of the engine (`opt_cache_snapshot`), set at quiescence
like any other input, so it is stable for the whole build and recorded as a dependency by exactly
the queries that read it. For each function, `codegen` asks `best_available(key, B, S)`: if `S`
holds a result at level `≥ B`, use it (the highest level present); otherwise compute
`optimized(key, B)` now. Rule: the result used is always the cached one with the highest level,
and never one *below* `B`. The OC is written only by the daemon, at quiescence, from the
`optimized` cells a build or the background work produced; a query never writes it (that would be
an effect). Two consequences, both intended:
a development build gets better over time as background work lands, and two machines with
different OC contents can produce different, individually correct objects. That is the trade
DECISIONS §4.8 makes; what we add is that the dependence is explicit and recorded, so the
incremental == cold invariant holds *given the same `S`*, which the test harness pins (§A9).

#### A8.4 Replay for releases

`ycxx --export-opt-cache` writes the subset of `S` a build actually used (the `best_available`
cells' dependencies tell us exactly which) as a replay manifest: `(key, level, result fp, result,
evidence)`. A release build with `--replay manifest` sets `S` to the manifest and `B` to the
manifest's levels, so its output is a function of inputs and a committed file, bit-identical on
any machine. Replay re-verifies each result's evidence (cheap: the evidence is a certificate, or
the rule set is proven once per optimizer version) rather than trusting the manifest.

Rejected: letting background work mutate results in place during a build (output would depend on
timing); keying optimizations by wall-clock budget (irreproducible); treating the OC as outside
the query model (then its reads would be untracked and incremental == cold would silently fail).

### A9. Enforcing incremental == cold

The invariant (DECISIONS §4.2): for any edit sequence, the incremental state after the last edit
equals a cold run on the final inputs. The harness, `tools/test incremental`:

**Edit generators.** (a) Random token-level edits: insert, delete, rename an identifier, flip a
`#if`, add or remove an include, touch a system header in a copied sysroot. (b) Edits mined from
git history: consecutive commits of libycxx and of ycxx itself replayed as revisions, which gives
realistic edit shapes. (c) Adversarial: delta-debugging over a failing edit sequence to find the
minimal sequence that still diverges, then saved as a regression test. (d) Regrouping: the same
edits applied one per revision, all in one revision, and in random batches; all must agree.

**What is compared.** Objects and linked outputs byte for byte; the sorted diagnostic report; and
the memo tables. The table comparison takes the cold run's set of `(key fp, value fp)` pairs and,
for each key, asks the incremental daemon `ensure_green(key)` at the final revision and compares
the resulting fingerprint. This is the right test because §A1.5 makes "valid at `R`" equal to the
cold run's reach, while `verified_at == R` is a subset of it (the durability shortcut does not
visit dependencies). The incremental table may hold extra stale cells; it may not answer a cold
key with a different fingerprint, and every answer must come back green without executing
anything (a counter of executions during the comparison must stay at zero). Comparing the table
catches a missed dependency even when no output depends on the wrong value today.

**Thread counts.** Each comparison runs at 1 thread, 2, `P`, and `2P` with the chaos scheduler on,
with the seed logged. `--replay-seed` reproduces a scheduling exactly: the chaos scheduler's
decisions are a function of the seed and a per-worker counter, not of time.

**Missed-dependency detector.** Debug builds have `--verify-memo`: at the end of a run, every green
cell is re-executed from scratch (dependencies force-fetched, so the recomputation is also
tracked) and its fingerprint and its dependency list compared with the stored ones. A mismatch
names the query kind and the key; a dependency list that differs names the fetch that was missed.
This is the direct test of "queries record what they read"; the memory policy of §A6.2 step 1
performs the same check opportunistically in release builds. For contextual queries the fresh run
bypasses the trie and the stored path is compared element by element with the fresh trace
(§A1.9), so a nondeterministic question order is caught as well as a missed dependency.

**Cycle and cancellation tests.** Every corpus program runs with cancellation injected at random
fetch counts followed by a resume; the final state must equal the uninterrupted run. Any ICE
(including a query cycle) fails the test.

**Optimization cache pinning.** Incremental tests run with a frozen `S` (an empty OC, or a fixed
manifest), since §A8.3 makes outputs a function of `S` by design.

### A10. The API a query author sees

The engine is written the way DECISIONS §3.2 asks: concepts constrain what a query kind, key and
value are; reflection derives hashing, equality, fingerprints and cache serialization from the key
and value types themselves; annotations carry query traits; contracts state the engine's
invariants; coroutines and `std::execution` carry the futures. Everything below compiles in spirit
on GCC 16.2 (§A10.4 says what was actually probed).

#### A10.1 Keys, values and kinds: concepts, derived by reflection

```cpp
export module ycxx.engine;                 // partitions: :cell :sched :coro :trace :persist :verify
import std;

export namespace ycxx::engine {

// A key is a structural aggregate: handles (strong enums over interned ids), integers, and nested
// structural aggregates. One consteval predicate decides it, recursively, by reflection.
consteval bool structural(std::meta::info t) {
  if (std::meta::is_enumerable_type(t) || std::meta::is_integral_type(t)) return true;
  if (!std::meta::is_aggregate_type(t) || !std::meta::is_trivially_copyable_type(t)) return false;
  for (auto m : std::meta::nonstatic_data_members_of(t, std::meta::access_context::unchecked()))
    if (!structural(std::meta::type_of(m))) return false;
  return true;
}
template<class T> consteval auto members() {
  return std::define_static_array(
      std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));
}

template<class T> concept QueryKey   = structural(^^T) && std::equality_comparable<T> && sizeof(T) <= 32;
template<class T> concept QueryValue = requires(const T& v, Sink& out) {
  { fingerprint(v) } -> std::same_as<Fp128>;   // Merkle for handles, structural for aggregates
  { serialize(v, out) };                        // derived below for structural types, hand-written for interned ones
};

// Fingerprint, equality, serialization: one definition each, for every structural type, forever.
template<class T> requires (structural(^^T) && std::is_class_v<T>)
constexpr Fp128 fingerprint(const T& k) {
  Fp128 h = type_seed<T>();                    // consteval hash of display_string_of(^^T): part of the schema
  template for (constexpr auto m : members<T>()) h = mix(h, fingerprint(k.[:m:]));
  return h;
}
template<class T> requires (structural(^^T) && std::is_class_v<T>)
void serialize(const T& k, Sink& out) { template for (constexpr auto m : members<T>()) serialize(k.[:m:], out); }
template<class T> requires (structural(^^T) && std::is_class_v<T>)
T deserialize(Source& in) { T k; template for (constexpr auto m : members<T>()) k.[:m:] = deserialize<typename [:std::meta::type_of(m):]>(in); return k; }

// Traits are annotations (P3394). An annotation's type is const-qualified, so look up by type.
struct cheap_t {}; struct transparent_t {}; struct contextual_t {}; struct context_t {};
struct input_t { Durability durability; };
inline constexpr cheap_t cheap{}; inline constexpr transparent_t transparent{};
inline constexpr contextual_t contextual{}; inline constexpr context_t context{};
struct demanded_t {}; inline constexpr demanded_t demanded{};   // instantiate_*, satisfies, constant_value (§A1.7)
template<class Q, class Trait>
concept has_trait = std::meta::annotations_of_with_type(^^Q, ^^Trait).size() > 0;

template<class Q> concept CheapQuery = QueryKey<typename Q::Key> && QueryValue<typename Q::Value>
  && has_trait<Q, cheap_t>
  && requires(SyncCtx& cx, typename Q::Key k) { { Q::run(cx, k) } -> std::same_as<typename Q::Value>; };
template<class Q> concept AsyncQuery = QueryKey<typename Q::Key> && QueryValue<typename Q::Value>
  && !has_trait<Q, cheap_t>
  && requires(Ctx& cx, typename Q::Key k) { { Q::run(cx, k) } -> std::same_as<query_task<typename Q::Value>>; };
template<class Q> concept InputKind  = QueryKey<typename Q::Key> && QueryValue<typename Q::Value> && has_trait<Q, input_t>;
template<class Q> concept DemandedQuery = AsyncQuery<Q> && has_trait<Q, demanded_t>;   // fetched only through cx.demand
template<class Q> concept Query      = CheapQuery<Q> || AsyncQuery<Q> || InputKind<Q>;

// The schema: kind ids are positions in this list; the schema hash (§A7.1) is a consteval fold over
// each kind's name, Key and Value member names and types. Adding a field changes the cache key.
template<Query... Qs> struct kinds { static constexpr Fp128 schema_hash = fold_schema<Qs...>(); };

}  // namespace ycxx::engine
```

Any key member of type `Prefix` that is the computation's context is marked `[[=context]]`, on
contextual kinds and on question cells alike, at any nesting depth (the `CtxKey` aggregate below
carries it): in trace patterns the engine replaces every such member that equals the fetching
frame's own prefix with the placeholder (§A1.9), so the C++ layer never spells out which member is
the prefix. The `pinned` and `evaluating` members of a `CtxKey` are ordinary key data and are never
abstracted. A `demanded` kind cannot be fetched with `fetch`; the
`DemandedQuery` constraint on `cx.demand` and its absence from `fetch` make a missing request
`Loc` a compile error.

#### A10.2 The context handle, with contracts

```cpp
export namespace ycxx::engine {

class Ctx {                                   // one per frame; the only way to observe the world
public:
  template<AsyncQuery Q> requires (!DemandedQuery<Q>)
  fetch_awaitable<Q> fetch(typename Q::Key k) noexcept;
  template<DemandedQuery Q>
  fetch_awaitable<Q> demand(typename Q::Key k, Loc at) noexcept;   // fetch + (index, at, fp) into the demand list
  template<Query Q> CellRef cell_of(typename Q::Key k) const noexcept;   // a key for DiagsOf; records nothing
  template<CheapQuery Q> const typename Q::Value& fetch(typename Q::Key k) noexcept;
  template<InputKind Q>  const typename Q::Value& fetch(typename Q::Key k) noexcept;
  template<AsyncQuery... Q> par_fetch_awaitable<Q...> par_fetch(q<Q>... qs) noexcept;   // when_all contract, positional
  template<AsyncQuery Q, std::ranges::input_range R> par_map_awaitable<Q> par_map(R&& keys) noexcept;
  template<class... A> void diag(DiagId id, A&&... args);
  Prefix prefix() const noexcept pre(contextual_);                 // only a contextual frame has one
  CtxKey ctx() const noexcept pre(contextual_);                    // (prefix, pinned, evaluating): passed on unchanged
  std::inplace_stop_token stop_token() const noexcept;
  Arena& scratch() noexcept;                                       // frame-owned, survives migration
  CellId self() const noexcept;
};

class SyncCtx {                               // for cheap kinds: inputs and cheap kinds only
public:
  template<class Q> requires (CheapQuery<Q> || InputKind<Q>)
  const typename Q::Value& fetch(typename Q::Key k) noexcept;
};

// The engine's own invariants are contracts, not asserts. A few that the design states in prose:
void Cell::publish(const Fp128 fp, const ValueRef v, const Revision r) noexcept
  pre(state() == State::Running) pre(owner() == current_frame())
  post(state() == State::Done) post(verified_at() == r) post(changed_at() <= verified_at());
bool Cell::maybe_changed_after(const Revision since, const Revision r) noexcept
  pre(since <= r) post(ok : !ok || changed_at() > since);
void Db::set_input(auto key, auto value, const Durability d)
  pre(live_frames() == 0);                                         // revisions advance only at quiescence
void Trie::insert(const Path& fresh, Leaf l)
  pre(fresh.starts_with(path_to(claimed_node_)));                  // the determinism check of §A1.9

}  // namespace ycxx::engine
```

A coroutine may carry preconditions but no postconditions (P2900), so a `query_task`'s
"postconditions" are `contract_assert`s before `co_return`. Development and CI builds compile the
engine with `-fcontract-evaluation-semantic=enforce` and a violation handler that prints the
query kind and key and aborts as an internal compiler error; release builds use `quick_enforce`,
which keeps the checks and drops the handler. One semantic per build, never mixed across TUs.

#### A10.3 Queries, as an author writes them

```cpp
export module ycxx.sema.lookup;
import std;
import ycxx.engine;
using namespace ycxx::engine;

// An input kind: durability travels with the kind. Set by the daemon, never read from disk in a query.
struct [[=input{Durability::Low}]] FileText { using Key = PathId; using Value = Blob; };

// A question cell (cheap, synchronous): Part B's real signature (§B2). The prefix is ordinary key
// data, marked [[=context]] so that traces abstract it. Keys of this shape number in the millions.
struct [[=cheap]] VisibleDecls {
  struct Key { ScopeId scope; NameId name; [[=context]] Prefix at; bool operator==(const Key&) const = default; };  // 12 bytes
  using Value = DeclList;                                       // interned handle; empty is a value
  static Value run(SyncCtx& cx, Key k) {
    const auto& lists = cx.fetch<PrefixItemLists>(k.at);        // hash-consed item lists of the prefix
    return lists.decls_named(k.scope, k.name);                  // through the syntactic indexes (§B1.3)
  }
};

// A transparent helper: not memoized; its fetches land in the caller's dependency list.
struct [[=transparent]] BodyTokenCount {
  struct Key { ItemId item; bool operator==(const Key&) const = default; };
  using Value = std::uint32_t;
  static Value run(SyncCtx& cx, Key k) { return cx.fetch<BodyTokens>(k.item).size(); }
};

// An ordinary kind: a coroutine. The hit path of co_await never suspends and allocates nothing.
struct AnalyzeTu {
  using Key = TuId;  using Value = TuSummary;                   // includes the TU's first-demand table (§B6.2)
  static query_task<Value> run(Ctx& cx, TuId tu) {
    auto [tree, cfg] = co_await cx.par_fetch(q<TuTree>(tu), q<Config>(tu));        // one group, argument order
    auto items  = co_await cx.fetch<TuItems>(tree);
    auto bodies = co_await cx.par_map<AnalyzeBody>(items.function_bodies());         // vector in key order
    if (bodies.size() > cfg.limit) cx.diag(Diag::TooManyFunctions, Loc{items.first(), 0}, cfg.limit);  // a value
    co_return summarize(items, bodies);
  }
};

// The TU's diagnostics are a separate kind over AnalyzeTu's cell: a cell fetching DiagsOf of itself
// would be a cycle (§A2.1). This is where Locs are rendered and chains attached (§A1.7, §A5.2).
struct TuDiagnostics {
  using Key = TuId;  using Value = RenderedDiags;
  static query_task<Value> run(Ctx& cx, TuId tu) {
    auto summary = co_await cx.fetch<AnalyzeTu>(tu);
    auto own     = co_await cx.fetch<DiagsOf>(cx.cell_of<AnalyzeTu>(tu));            // program order, stops at demanded kinds
    co_return render(cx, tu, own, summary.first_demand_table());                     // + demanded cells' diags with chains
  }
};

// A contextual, demanded kind: index + prefix, instantiated at the demanding site's own prefix (§B6.2).
// The engine walks the per-index trie before running this (§A1.9); tu(TU) does the END comparison.
// The context of every contextual kind (§A1.9, §B1.4): the prefix is abstracted by the trie; the two
// maps are interned handles, ordinary key data, empty handles in every ordinary demand.
struct CtxKey { [[=context]] Prefix prefix; PinnedId pinned; EvaluatingId evaluating;
                bool operator==(const CtxKey&) const = default; };                          // 12 bytes
struct [[=contextual, =demanded]] InstantiateClass {
  struct Key { EntityId spec; CtxKey ctx; bool operator==(const Key&) const = default; };   // 16 bytes
  using Value = ClassDef;
  static query_task<Value> run(Ctx& cx, Key k) {
    if (auto first = pinned_lookup(k.ctx.pinned, k.spec))                                  // this TU pinned S (§B6.2):
      co_return co_await cx.fetch<InstantiateClass>({k.spec, *first});                     // reuse that cell, ordinary dependency
    auto specs = co_await cx.fetch<SpecializationsOf>({k.spec.primary(), cx.prefix()});    // question @ own prefix (§B6.3)
    auto tmpl  = co_await cx.fetch<ClassTemplateDef>(k.spec.primary());                    // context-free, still traced
    co_return co_await instantiate(cx, tmpl, specs);
  }
};

// Demanding one, from a body analysis. The context is passed on unchanged; the request Loc goes into
// *this* cell's demand list (a value): not into the key, not onto an edge. tu(TU) builds the
// first-demand table and "required from here" chains from demand lists.
auto def = co_await cx.demand<InstantiateClass>({spec, cx.ctx()}, Loc{item, offset});
// Context-dependent outcomes are values, not diagnostics: the consumer decides.
if (auto r = co_await cx.fetch<Substitute>({tmpl, args}); r.failed()) { /* SFINAE: discard r.reason() */ }
```

```cpp
// The daemon's edge: std::execution::task, a counting_scope of roots, stop sources per root.
std::execution::task<void> Daemon::on_build(BuildRequest r) {
  auto root = db.request<BuildReport>(r.build, Priority::Background);    // a query_task: sender + awaitable
  std::execution::spawn(std::move(root) | std::execution::then([&](const BuildReport& b) { reply(b); }),
                        roots_, root_env(r.stop_source));
  co_return;
}
std::execution::task<void> Daemon::apply_edits(EditBatch b) {
  for (auto& s : live_stop_sources_) s.request_stop();
  co_await roots_.join();                                                 // quiescence (§A4.1)
  db.set_inputs(b);                                                       // pre(live_frames() == 0)
}
```

`Ctx` offers exactly what the listing shows and nothing else; there is no way to read an input
except `fetch`. Query code may throw its own types across its own helpers but never out of `run`:
a query's failure is a value in its `Value` type, and an escaping exception is an internal compiler
error (§A4.3).

#### A10.4 Bootstrap: what GCC 16.2 does, what it does not, and the workaround for each

Probed with `g++-16 -std=c++26 -freflection -fcontracts -fmodules` on macOS arm64 (Homebrew GCC
16.2.0), libstdc++ only. libycxx ships `<meta>`, `<contracts>`, `<coroutine>`, `<stop_token>`,
`<execution>` with P2300, P3552 `task` and `parallel_scheduler_replacement`, and a `std` module,
so none of the engine's includes need a `// BOOTSTRAP:` fallback to libstdc++.

| Feature the design leans on | GCC 16.2 | Risk, and the smallest workaround if it bites |
| --- | --- | --- |
| `nonstatic_data_members_of` + `define_static_array` + `template for` + `[:m:]` splices (fingerprint, equality, serialization) | Works, including nested aggregates and `typename [:type_of(m):]` | Expansion over a non-static range is not supported by the paper either; always go through `define_static_array`. |
| Annotations (P3394) on classes, members, static members, variables, functions, enumerators, namespaces; `annotations_of_with_type`, `extract` | All positions work | `type_of(annotation)` is `const T`, so `type_of(a) == ^^T` is false; use `annotations_of_with_type` (done above). Annotations on template specializations and across module boundaries are unprobed; `// BOOTSTRAP: annotations: static constexpr traits member read by static_data_members_of` if they fail. |
| Reflection inside a module unit, `import std`, a consumer reflecting through an imported template | Works | Reflecting an *exported* class from another module unit is unprobed; low risk, checked in the engine's first commit. |
| Contracts: `pre`, `post(r: …)`, `contract_assert`, user `handle_contract_violation`, `-fcontract-evaluation-semantic=ignore|observe|enforce|quick_enforce` | Work; default is `enforce` (handler, then terminate) | Parameters named in `post` must be `const` (the paper requires it; GCC enforces it): engine signatures with postconditions take `const` parameters. Postconditions on coroutines are ill-formed: use `contract_assert` before `co_return`. Mixing semantics across TUs is permitted by P2900 but unprobed on GCC; use one per build. |
| Coroutines: promise `operator new` with a leading `Ctx&`, symmetric transfer via `noop_coroutine`, `await_ready` fast path, `pre` on a coroutine | Work | No HALO at `-O2` (a child awaited by its parent still allocates), so the budget of §A3.3 assumes one free-list allocation per executed query. If frame sizes surprise, the open item in Part E caps them. |
| `std::execution` senders, `task`, `parallel_scheduler_replacement`, stop tokens | Absent from libstdc++ 16; present in libycxx and tested there on GCC 16.2 under TSan | ycxx builds against libycxx, so nothing to work around. The two libycxx gaps that touch the daemon's edges (per-construction backend allocation in `task_scheduler`, no completion domain from `spawn_future`) are avoided as Part E says. |
| `import std` | Works once the std module is built (`-fmodules -fsearch-include-path -fmodule-only`) | Build system must build libycxx's `std.cppm` first; a `// BOOTSTRAP: modules: header import` fallback is not expected to be needed. |

## Part B. The C++ layer: what is a query, what is its key, what it records

*Drafted by a second Fable subagent, revised after review and reconciled with Part A (see
Provenance).* Part B decides which queries exist, how they are keyed so that an edit disturbs only
the questions whose answer could change, and what each semantic step records. It runs on Part A;
the one capability it needs beyond the salsa model, contextual queries, is specified in §A1.9.

### B0. Part B in one screen

1. **The unit of source identity is the item**: a top-level or member declaration cut from the
   preprocessed token stream before parsing. Its id is derived from the identity of its file, its
   own tokens (declaration part only, bodies excluded), its syntactic scope path and an ordinal
   among identical siblings. Not from its position, not from the content of the enclosing file.
2. **A program point is `(item, offset-in-item)`.** Visibility of a declaration at a point is a
   question `visible_decls(S, name, at: prefix)`, answered from shared syntactic indexes of the
   item lists and trees that make up the prefix. Absolute positions never appear in a key; a
   location in any value is `(item, offset)` and is rendered to file, line and column by `tu(TU)`
   (§B2).
3. **Semantic queries are contextual.** Their key is `(index part, prefix)`, where the prefix is
   an interned, content-addressed description of everything that textually precedes the point
   (§A1.9); no TU appears in any semantic key. The results for one index part are
   shared through a trie of *traces*: the complete dependency list of the computation, with the
   questions it asked of its prefix abstracted over the prefix. A fetch at a new prefix walks the
   trie re-asking each question and lands on an existing result whenever every answer matches.
   This is the mechanism that shares headers, instantiations and constant evaluations across
   translation units with different preambles (§B1.4).
4. **Two levels of identity:** syntactic *declaration ids* (what lookup returns) and structural
   *entity ids* ([basic.link]-style: scope, name, signature) that are program-wide and key
   instantiations, layouts, IR and ODR checks.
5. **The function body is the smallest unit of analysis.** Inside a body, lookups, overload
   resolutions, conversions, instantiations and constant evaluations are queries (recorded and
   shared); individual expressions are not.
6. **Function template definitions are instantiated in the end-of-TU context**
   ([temp.point]/7); class and variable template specializations, concept satisfaction and
   anything needed for constant evaluation are instantiated **at the prefix of the demanding
   site**, and specialization selection uses the specializations visible at that prefix. No
   query computes "the point of first demand": every selection-cell value carries the list of
   specializations it demanded directly, and `tu(TU)` traverses those lists in source order to
   build the TU's first-demand table. A later site of the same TU that selected a different
   result for a class template specialization is repaired by re-fetching it with a `pinned` key
   argument that routes the specialization to the first demand's cell, so the TU gets the one
   instantiation [temp.point]/7 prescribes; the same divergence for variable templates, static
   data members and satisfaction is IFNDR and is diagnosed (§B6.2). Across TUs,
   `odr_report(program)` compares the results each TU selected (§B6.6, §B10).
7. **The region index is syntactic.** Per preprocessed file occurrence, "which items declare
   `name` in scope path `P`", friend declarations, using-directives, specializations and
   definitions are indexed without semantics, so that invalidation of lookups is decided by hash
   lookups, not by re-parsing.

### B1. Vocabulary and the engine contract

#### B1.1 Regions, trees, prefixes

Preprocessing a file occurrence yields a **region**: its own token stream (with `#include`
directives replaced by references to child regions), the macro delta it produced, its `#pragma
once` effects and its `__COUNTER__` delta. A region and its children form a **tree**; the tree id
is the fingerprint of the *complete* `pp` results of the tree, not of the token streams alone, so
that two configuration headers whose only difference is what they `#define` have different tree
ids and different prefixes. A TU is the tree rooted at the main file.

A program point's context is its **prefix** (§A1.9): for each level of the include
path, the item list of that region up to the include item that leads down, with include items
represented by the tree id they bring in; at the leaf level, the items of the point's own region
up to the item. A prefix node is `(parent prefix, item-list handle)`, hash-consed, so two TUs that
include the same headers in the same order up to a point share the prefix handle without anyone
comparing anything. The END context of a TU is the prefix of its whole tree. Prefixes appear in
the keys of question cells and selection cells; tree ids and item-list handles appear in keys of
syntactic queries (§B1.3). Neither a TU nor an include-occurrence path appears in any semantic key;
the include chain is still recoverable from a prefix for diagnostics (`item_location(item,
prefix)`).

#### B1.2 Items and declaration ids

A region's token stream is split into **items** by a token-level pre-pass (depth-0 `;`, depth-0
`}` with a small lookahead for declarator lists after a class body, `template<...>` prefixes
attached to what follows, `namespace {}`, linkage specifications and class bodies forming nested
item lists). The parser later confirms the boundaries; a disagreement is a bug in the splitter,
never a soundness issue, because the parser works from the item sequence.

Each item has a **declaration part** (everything except a function body, `= default`, `= delete`,
a function-try-block) and, for function definitions, a **body part**. The item id is

    item_id = hash(file identity, syntactic scope path, declaration-part tokens, ordinal)

where the ordinal counts earlier siblings with the same hash (two `void f(int);` redeclarations),
and the syntactic scope path is the spelling of the enclosing `namespace`/class nesting
(`std::__1::vector`). When an identical region occurs twice before a point (an X-macro header
included twice with the same macro state), the second occurrence's items are salted with their
occurrence count *in the prefix*; the salt is a function of the prefix, so it is TU-free and the
one-leaf property of the trie holds.

Consequences: editing a body does not change the id of the function, so callers keep their recorded
dependencies; editing a declaration (`void f(int)` → `void f(int) noexcept`) changes the id, which
is right because every use must be re-checked; adding or removing an unrelated item in the same
file changes no id. `body_tokens(item)` is a separate query, so bodies are inputs of their own.

#### B1.3 Syntactic indexes

Per item list (`R` below is the content-addressed handle of a region's item list, which is what a
prefix node carries; non-contextual, pure, cheap):

- `region_items(R)`: the item tree, in order.
- `region_decls_named(R, P, name)`: ordered items declaring `name` into scope path `P`
  (including unscoped enumerators into the enclosing scope, injected-class-names and
  using-declarations under the name they introduce; `using namespace` is not a declaration of a
  name and is indexed separately). Friend declarations are indexed
  separately in `region_friends(R, P_class, name)` because ordinary lookup must not see them
  ([class.friend]).
- `region_using_directives(R, P)`, `region_inline_namespaces(R, P)`,
  `region_specializations(R, template path)` (explicit and partial), `region_definitions(R, P, name)`
  (definitions, including qualified out-of-line ones indexed under the *spelled* qualifier),
  `region_names(R)`: the set of all names declared anywhere in `R` (a filter).

Per tree, the same families aggregated over the subtree in order (`tree_decls_named(T, P, name)`
etc.), descending only into children whose `tree_names` contains `name`. These are shared by every
TU that includes the same preprocessed header. Because the keys are content hashes, an edit makes
*new* keys; the old entries become garbage. That is acceptable because these queries are
recomputed in microseconds and are reached only from question cells whose own key, the prefix,
pins the same content-addressed handles (§B1.4).

**Decision (adopted by §A1.9):** a trace never names a syntactic index directly. Every
prefix-dependent read goes through a question cell keyed by `(question, prefix)`, and the trace
element is that question with the prefix abstracted. Context-free dependencies that are keyed by
stable ids (`body_tokens(item)`, `item_tokens(item)`, pure matchers such as `match_partial`,
other selection cells) appear in the trace as they are. A content-hash key in a trace element
would never match in another prefix whose equivalent tree has a different hash, which is exactly
the case the trie exists to share.

#### B1.4 Contextual queries and traces: the engine contract

This is the contract of §A1.9, restated from the C++ side. A **contextual query** has
key `(index, prefix)`. Its cell is a *selection cell* whose value is the shared result selected
for this prefix. Per index, the engine keeps a **trie** of traces. A trace is the computation's
*complete* dependency list in program order: the **questions** it asked of its prefix
(`visible_decls`, `macro`, `complete_before`, `definition_item`, ... each a question cell keyed by
`(question, prefix)` whose answer is a function of the preceding tokens) with the prefix replaced
by a placeholder, and its context-free dependencies (`body_tokens(item)`, pure matchers, other
selection cells it fetched) as they are. A fetch walks the trie from the root, re-asking each
question at its own prefix; when every answer matches a stored path it returns that leaf's result
and records the path as its dependencies; at the first missing edge it runs the computation at
this prefix and inserts the new path. Because the computation is deterministic and reads nothing
untracked, at most one leaf can match; the trie is a pure cache and may be emptied without
changing any output.

Two consequences shape everything below. A result may mention declaration ids, entity ids and
item-relative positions, never a TU or a prefix, or two prefixes with identical traces
would hold different results. And a computation asks questions only at its own prefix; an
entity-keyed query asked at prefix `Q` (`class_def(C, Q)`, `instantiate_fn(spec, END)`) locates
the defining item with a question (`definition_item(C, Q)`) and fetches that item's analysis at
the item's own prefix, which is a sub-prefix of `Q`, as an ordinary dependency.

Notation: in the rest of this document `ctx` in a query signature is the context argument
`Ctx = (prefix, pinned, evaluating)`: the prefix, which the trie abstracts, and two small maps that
are ordinary key data and empty in every ordinary demand (`pinned`, §B6.2; `evaluating`, §B7).
Every fetch a contextual computation makes passes its own `ctx` on unchanged, except that
`evaluating` grows along a constant-evaluation chain. `at: I` abbreviates the context whose prefix
is that of item `I`; `complete_before(C, at I)` is therefore the question cell `complete_before(C,
prefix-of-I)` (questions take the prefix alone).

**Demand lists.** The value of every selection cell carries, besides its result, the ordered list
of *direct demands* it made: for each instantiation, satisfaction check or constant evaluation it
fetched, the index, the `(item, offset)` at which it was requested, and the fingerprint of the
result it selected. Nested demands are in the demanded cell's own list, so `tu(TU)` reaches the
transitive set by traversal. The list is a value like any other (fingerprinted, persisted with
the leaf), so it is what "required from here" chains, emission sets and the first-demand table
of §B6.2 are built from. The engine appends the entry when a demand is made with
`cx.demand<Q>(key, loc)` (§A1.7).

Divergence (two leaves under one index with different results) is not reported from the trie:
`tu(TU)` emits the list of entity-keyed indexes it demanded with the result fingerprint selected
for each, and `odr_report(program)` (§B10) merges those lists. The trie's contents, garbage
collection timing and edit history therefore cannot change a diagnostic.

This is the "verifying traces" construction of Mokhov, Mitchell and Peyton Jones, *Build Systems
à la Carte* (ICFP 2018), with the dependency keys abstracted over the prefix. Why not fold the
context into the key? Because the context is not a value the caller can hash without knowing what
the callee will ask. A header's analysis depends on `macro(_LIBCPP_X)` and `visible_decls(std,
__construct_at, at ...)` but not on the ten thousand other things in the preamble; only the trace
knows which.

#### B1.5 Entity ids

`entity_of(decl, ctx)` maps a declaration to a structural id: for namespaces, the canonical path;
for classes and enumerations, `(scope entity, name)`; for functions, `(scope entity, name,
canonical function type, template head)`; for specializations, `(template entity, canonical
arguments)`. Entity ids never change when a redeclaration is added. They key everything
program-wide: `class_def`, `layout`, `function_ir`, `instantiate_*`, `satisfies`, ODR.

Entities with internal linkage, members of unnamed namespaces and local entities get ids that are
**TU-free**: `(scope entity, name, declaration id of the first declaration)` for the first two
(the unnamed namespace itself is identified by the declaration id of its first
*namespace-definition*), `(enclosing entity, ordinal)` for local entities. The same `static`
helper in a header therefore has one id in every TU, which is what lets results that mention it be
shared; the standard's rule that such declarations in different TUs are different entities
([basic.link]/8, /9) is applied where it is observable: `tu(TU)` gives their symbols a TU-local
name at emission, and `odr_report` skips indexes whose entity has internal linkage or is
TU-local. No TU appears in any entity id.

### B2. Program points and visibility

Lookup sees declarations that precede the point ([basic.lookup.general], [basic.scope.scope]).
The question "which declarations of `name` in scope `S` precede item `I`" is the query

    visible_decls(S, name, at: prefix)  →  ordered list of (declaration id, flags)

a question cell computed by walking the prefix from the root: at each level,
`region_decls_named(R, S, name)` over the level's item-list handle `R` (the items before the
include item), with each earlier include item contributing `tree_decls_named(tree id, S, name)`;
at the leaf, the item's own region's items before it. All of these are the shared syntactic
indexes of §B1.3, so a
question cell is a handful of hash lookups and holds only content-addressed dependencies. `S` is a
namespace entity id mapped to the scope paths that spell it (its canonical path; inline namespaces
are handled in §B3). Lookups into inline namespaces, nominated namespaces and bases are separate
`visible_decls` fetches, so each is recorded separately.

What this buys: the answer is a list of stable ids. Adding `void f(long)` to `N` in header H
changes `region_decls_named(R_H, N, f)` and therefore `visible_decls(N, f, at I)` for every `I`
after the insertion point, and nothing else: items that never asked about `f` in `N` have no such
entry in their trace. Items *before* the insertion point are not disturbed either, because
their prefixes do not contain the new item. A body edit elsewhere in the same file gives new
prefixes to the items after it; their selection cells re-walk the trie, each question answers in
O(1), each answer hash is equal, every walk lands on its old leaf.

**Locations and source order (adopted by §A1.7 and §A5.2).** A location in any value, diagnostics
included, is `Loc = (item id, offset)`: the offset counts tokens (or characters) from the start of
the item's own tokens, so a `Loc` is a function of the item's content and is shared across TUs and
prefixes. It is not an ordering key on its own and cannot be rendered on its own. `tu(TU)` renders
it: the TU's tree gives the ordered sequence of its items, each with the file identity of its
region and the item's offset in that file (the regions' token streams carry file offsets); from
these `render(Loc, TU) = (file, line, column, include chain)`. Source order within a TU is the
order of items in that sequence, then the offset; a diagnostic's sort key is that position, and
duplicates of a shared item's diagnostic across TUs are merged by the driver on the rendered
`(file, line, column, id, args)`. The "required from here" chain of an instantiation diagnostic is
the sequence of request positions along the demand lists (§B1.4) from the TU's item to the
instantiation, each rendered the same way. Equal `Loc`s in different items are different
positions because the item id differs; an item that occurs twice in a TU is distinguished by its
prefix-derived salt (§B1.2).

What the AST must provide: items as the unit of parsing; source positions stored as `Loc`;
declaration ids and entity ids as the only cross-item references.

Rejected: absolute offsets as keys (every edit above shifts them); "the scope's member list" as
the dependency (any addition to `std` would invalidate everything); order-maintenance labels
maintained as inputs (impure, TU-specific, and they leak into results, which kills cross-TU trace
matching); per-TU prefix chains over all nested regions (O(regions × names) nodes per TU; the
include-tree aggregation above gives O(depth × names) per distinct prefix and shares the rest);
`(TU, position)` as the context (multiplies every cell by the TU count; see §A1.9).

### B3. Unqualified and qualified lookup

Unqualified lookup of `name` at item `I` in scope chain `S0 ⊂ S1 ⊂ … ⊂ ::`
([basic.lookup.unqual]):

```
for S in chain, innermost first:
  found = scope_lookup(S, name, at I)
  if found nonempty: return found       // hiding: outer scopes are not fetched
return ∅                                // negative result, recorded through the fetches above
```

Each `scope_lookup` is one recorded question per scope level. Hiding falls out: if `N::f` exists,
`::f` is never asked, and adding `::g` touches no lookup of `f`. Adding a hiding declaration
changes exactly the empty answer that the inner level had recorded:

```cpp
void f(int);
namespace N {
  // edit adds here:  void f(double);
  void g() { f(1); }    // recorded: visible_decls(N, f, at g) = ∅, visible_decls(::, f, at g) = {::f}
}
namespace N { void f(char); }   // after g: never affects visible_decls(N, f, at g)
```

**Namespace level.** `scope_lookup(N, name, at I)` is `visible_decls(N, name, at I)` ∪
`visible_decls(M, name, at I)` for each `M` in `inline_members(N, at I)` (namespaces declared
`inline` in `N` before `I`, transitively; [namespace.def.general]) ∪ the using-directive
contribution: for each using-directive visible at `I` whose nominated namespace `U` has `N` as the
nearest namespace enclosing both the directive and `U` ([namespace.udir]/3), `visible_decls(U,
name, at I)`. The set of directives is `directive_closure(at I)`: the using-directives in every
scope of the chain before `I` (block-scope ones are inside the body being analysed and cost no
query; namespace ones are `using_directives(Sk, at I)` per level), closed transitively through
`using_directives(U, at I)` for each nominated `U` ([namespace.udir]/5). The closure is one query
per item, shared by all names looked up from that item, and every lookup from `I` records it.

```cpp
namespace A { void h(int); }
namespace B { /* edit adds: using namespace A; */ }
namespace C { using namespace B; }
void k() { using namespace C; h(0); }
// recorded by k: using_directives(C, at k) = {B}, using_directives(B, at k) = ∅ (negative),
//                visible_decls(::|C|B, h, at k) = ∅  → error
// after the edit: using_directives(B, at k) changes → k re-executes → closure {C,B,A} → finds A::h
```

**Class level** (member function bodies, nested classes, member declarations):
`member_lookup(C, name, ctx)` implements [class.member.lookup]: the declaration set of `C` is
`class_decls_named(C, name)` from `class_def(C, ctx)` (§B8); if it is non-empty, bases are *not*
fetched (so a new member in a base cannot disturb a lookup that `C` itself satisfies); otherwise
the lookup sets of the direct bases are merged with the subobject rules, and ambiguity is a
*value* in the result, not an exception. Within the class definition but outside complete-class
contexts ([class.mem.general]), members are filtered to those before the member item:
`class_decls_named_before(C, name, member item)`. The rule that a search from a member declaration
must give the same result as a search in a complete-class context of the class
([class.member.lookup]/6) is IFNDR; `class_def(C)` diagnoses it by fetching, for each name its
member declarations looked up, the complete-class answer and comparing (§B6.6 explains the
technique).

**Block scopes, template parameter scopes, enumeration scopes.** Block-scope declarations are
inside the body being analysed and are plain data of that query; a hiding declaration added to a
block changes `body_tokens` and the body re-runs, which is both unavoidable and cheap. Template
parameter names are in the item's declaration part. Enumerators of a scoped enumeration are
members of the enumeration entity (`class_decls_named`-style, from `enum_def(E, ctx)`); unscoped
enumerators are indexed into the enclosing scope path by the region index. The injected-class-name
([class.pre]) is a synthetic member of every class.

**Using-declarations.** A `using N::f;` item is indexed under `f` in its own scope. What it
introduces is `using_decl_targets(d, ctx)` = the qualified lookup of `N::f` *at the
using-declaration's own position* ([namespace.udecl]); a later `N::f(long)` is correctly not
introduced, and the lookup that found the using-declaration depends on `using_decl_targets(d)`,
not on `visible_decls(N, f, at I)`.

**Qualified lookup** ([basic.lookup.qual.general]): `N::f` at `I` is `visible_decls(N, f, at I)`
∪ inline members; if empty, the namespaces nominated by using-directives *in N* before `I`,
recursively ([namespace.qual]). `C::m` is `member_lookup(C, m, ctx)` and additionally records
`complete_before(C, at I)` (§B8), so that a later definition of `C` invalidates exactly the
qualified lookups that found it incomplete.

Over-invalidation accepted here: `directive_closure` and `inline_members` are per item, not per
name (a new using-directive anywhere in the chain re-runs every lookup from items after it, each
answering in O(1) and cutting off); `class_decls_named` sees the class definition as one value
(§B8 says why that is fine).

### B4. ADL, hidden friends and friend injection

ADL ([basic.lookup.argdep]) is `adl(name, arg types, at I)`:

1. `associated(T, at I)` per argument type: namespaces and classes per [basic.lookup.argdep]/3,
   computed from the canonical type, its template arguments, function parameter and return types,
   and the bases of class types. It is contextual because bases are only known for a class that
   is complete at `I`: it records `complete_before(C, at I)` for each class type it meets and
   fetches `class_def(C, at I)` only when the answer is true; an incomplete class contributes
   itself and its namespace only. Its trie has one leaf per distinct completeness pattern, which
   in practice is one.
2. For each associated namespace `N`: `visible_decls(N, name, at I)`, **without** the
   using-directive contribution ([basic.lookup.argdep]/4).
3. For each associated class `C` that is complete at `I`: `friend_decls(C, name)` from
   `class_def(C)`, the friend functions and function templates declared in `C` that are members of
   their enclosing namespace but invisible to ordinary lookup ([class.friend], formerly
   [namespace.memdef]).

Items 2 and 3 are recorded separately, so the two edits below hit different entries:

```cpp
namespace N { struct S { friend void f(S); }; }
void g(N::S s) { f(s); }
// recorded: visible_decls(::, f, at g) = ∅, visible_decls(N, f, at g) = ∅,
//           friend_decls(N::S, f) = {hidden friend}
```

Adding `void f(S);` to `N` at namespace scope changes the second entry; `g` re-runs, resolution
picks the same entity (the friend and the namespace-scope declaration are one entity), the body
result is equal, cutoff. Adding `friend void f(S, int)` to `S` changes `class_def(N::S)` and thus
`friend_decls(N::S, f)`; only ADL lookups whose associated set contained `N::S` and that asked
about `f` re-run. A friend declared in a class that is *not* an associated class is reachable
only after a matching namespace-scope declaration, which the region index will then contain
([class.friend]).

Friend injection into a namespace is modelled as the region index listing the friend under the
class, never under the namespace; nothing is "injected" as state. ADL in templates at the point
of instantiation uses the same query at the instantiation's prefix (§B6.2).

### B5. Overload resolution

Resolution is a contextual query keyed by the call's observable inputs:

    resolve(kind, candidates, args, extras)  [ctx]
      kind       = call | member call (object type, cv/ref) | operator(op) | init(target, direct|copy|list)
                 | conversion(target) | deduction guide ([over.match.class.deduct])
      candidates = sorted set of declaration ids found by lookup and ADL (what the point sees,
                   including redeclarations that add default arguments, [dcl.fct.default])
      args       = per argument (canonical type, value category, bit-field?, null pointer constant?,
                   braced list → recursive shape), explicit template arguments
      result     = winner (entity + deduced arguments) | ambiguous {set} | none,
                   per-argument conversion sequence summary, diagnostic sketch

Using the exact declaration set as the index makes two calls to `std::move` with the same
argument type anywhere in the program hit one entry. When an overload appears, the lookup answer
changes, the body re-runs, and its `resolve` fetch has a *new* index (a bigger set) which is
computed once and then shared by every other call site with the same shape.

Internals, each a recorded fetch:

- `function_type(d, ctx)`, `default_args(d)`: from `decl_sema(d, ctx)` (§B11).
- `deduce(template decl, args, explicit args, ctx)` ([temp.deduct.call]); keyed by template entity
  and argument shapes, shared.
- `implicit_conversion(from, to, ctx)` ([over.best.ics]): standard conversions are pure; user
  defined ones fetch `converting_constructors(C, ctx)` and `conversion_functions(C, ctx)` from
  `class_def` ([over.match.copy], [over.match.conv], [over.match.ref]), derived-to-base fetches
  `bases(C)`, and all of them record `complete_before(C, at I)` when `C` is a class. List
  initialization ([over.match.list]) recurses into `resolve(init, ...)` for the element.
- Operators ([over.match.oper]): member candidates via `member_lookup(T1, operator@)`,
  non-member via unqualified lookup + ADL of `operator@`, built-in candidates ([over.built]) are
  pure; rewritten candidates for `==`, `!=`, `<`, `<=`, `>`, `>=`, `<=>` add the lookups of
  `operator==` and `operator<=>` to the trace, so a newly added `operator<=>` re-resolves exactly
  the comparisons that could use it.
- `best_viable` ([over.match.best], [over.ics.rank]): pure over the above; constraints on
  candidates fetch `satisfies` (§B6.5); `more_constrained` ([temp.constr.order]) is pure and
  memoized by the pair of constraint sets.

Propagation: the body result carries the chosen entity and conversion summary per call. If the
new overload does not win anywhere in the body, the body result is equal and nothing downstream
(IR, codegen, diagnostics) moves. If it wins, exactly that body's IR changes.

Rejected: keying resolution by the call expression (no sharing, millions of entries); keying by
entity set instead of declaration set (default arguments and visibility of redeclarations are
declaration properties); per-expression queries (memory; a body re-run is microseconds).

### B6. Templates

#### B6.1 Definition analysis: binding at phase one

`analyze_template(item, ctx)` parses and checks the template at its definition point
([temp.res.general]). Non-dependent names are looked up and *bound* to entities in the result.
For each dependent unqualified call, the result stores the definition-context unqualified lookup
set as a value ([temp.dep.candidate]: candidates are the definition-context lookup plus ADL from
both contexts). Dependent qualified names and member accesses are left symbolic. This is a
contextual query like any declaration's; its prefix is the template item's own.

#### B6.2 Instantiation and its context

    instantiate_fn(spec entity, prefix, evaluating = ∅)   prefix = END of the TU, or the demanding
                                                          site's prefix when needed for constant
                                                          evaluation ([temp.inst]/8)
    instantiate_class(spec entity, prefix)                prefix = the demanding site's prefix
    instantiate_var(spec entity, prefix, evaluating = ∅)  prefix = the demanding site's prefix

The index is the specialization's entity id (`(template entity, canonical arguments)`), so a
single trie serves the whole program. What the instantiation observed via ADL, partial
specializations, completeness and constant evaluation is in its trace. Two TUs whose prefixes
agree on every question walk to the same leaf and compute nothing. A TU whose preamble changes an
answer computes a second leaf; `tu(TU)` reports the fingerprint it selected, and `odr_report`
(§B10) diagnoses that the specialization has different meanings in the two TUs ([temp.point]/7
says that is ill-formed, no diagnostic required; we give one).

**Function templates** use the END context because the end of the TU is a point of instantiation
for every specialization that has one ([temp.point]/7), all points must agree or the program is
IFNDR, and END sees the most declarations, which is what the real-world corpus assumes of
compilers. `tu(TU)` demands them: it traverses the demand lists (§B1.4) of its items and fetches
`instantiate_fn(spec, END)` for every function template specialization whose definition is
required. The exception is a specialization needed for constant evaluation while parsing
([temp.inst]/8): the evaluating site fetches it at its own prefix and that cell is the one used
for code generation; `tu(TU)` also fetches the END cell and compares fingerprints; a difference
means two points of instantiation disagree, and we diagnose it ([temp.point]/7) instead of
silently choosing. The `evaluating` argument is the cycle-avoidance context of §B7 and is empty
except on a constant-evaluation chain.

**Class templates: the demanding site's prefix, the first-demand table, and pins.** A class
template specialization is implicitly instantiated when it is first referenced in a context that
requires it to be complete ([temp.inst]/2), it has at most one point of instantiation per TU
([temp.point]/7), and later uses in the same TU see that one instantiation. What the
instantiation can observe of its point is: the specializations of its template that are visible
(§B6.3), the completeness of the types its definition inspects (`sizeof(T)`, `T::type`, dependent
bases), and ADL for dependent calls in the class body. A sequential compiler finds the first
demand by having done everything before it. A query cannot: a demanding site asks questions only
at its own prefix (§B1.4), and a question `first_demand(spec, prefix)` would be an aggregation
over the *semantic* results of every earlier item, bodies included, making every item depend on
every earlier body and serialising the TU. There is no such question.

The rule has three parts.

1. *Every demanding site fetches `instantiate_class(spec, own context)`.* In a program whose
   meaning does not depend on the order of demands, every site's walk lands on the same leaf,
   because the questions (completeness of the inspected types, the specialization set, ADL
   answers) have the same answers at every site. This is the common case and costs nothing beyond
   the walk.
2. *`tu(TU)` builds the first-demand table.* It traverses its items in source order and, within
   each, the demand lists (§B1.4) in program order, recording `spec → (first demanding Loc, the
   context of the cell that demanded it, selected fingerprint)`. The first entry is the TU's point
   of instantiation for attribution and the "required from here" chain.
3. *Divergence is repaired by pinning.* The context of every contextual query is
   `Ctx = (prefix, pinned, evaluating)`, where `pinned` is a small sorted map `spec → Ctx` that is
   key data like `evaluating` (§B7), threaded unchanged into every fetch the computation makes,
   and empty in every ordinary demand. `instantiate_class(S, ctx)` with `S ∈ ctx.pinned` does not
   compute: it fetches `instantiate_class(S, ctx.pinned[S])`, the cell in which this TU first
   instantiated `S`, as an ordinary dependency, and returns its result. When `tu(TU)`, processing
   item `J`, finds in `J`'s demand list a class specialization `S` whose fingerprint differs from
   the table's earlier entry, it re-fetches `J`'s analysis with `pinned ∪ {S → first context}`
   and traverses the new demand list; it repeats until the list agrees with the table, then moves
   on. Later items inherit no pins unless they diverge themselves.

Why this is sound. The pin map is in the key, so `analyze(J, P_J, {S → c})` and `analyze(J, P_J,
∅)` are different indexes with their own tries; the one-leaf argument of §A1.9 is unchanged
because each is a deterministic function of its key and its recorded answers. A pin names a cell
(`spec` and the full context it was demanded in), not a fingerprint, so there is no
fingerprint-to-value lookup and an evicted value is recomputed at exactly the context that
produced it. Pins flow to nested demands because they are part of the context that every fetch
inherits: an `S` demanded inside `instantiate_class(X<T>, ctx)` or inside an
`instantiate_fn(spec, END-ctx)` sees the same `pinned`. The table is built in source order and
each item is repaired before the next is examined, so an earlier entry is always the corrected
one. Termination: each round adds at least one pin for a specialization `J` demands, a pin is
never removed, and the specializations `J` can demand are finite (instantiation depth is
bounded), so the rounds are bounded in practice by the number of divergent specializations,
which is one or two. Determinism: source order, sorted maps, no completion-order input.

What it costs. In the common case, nothing: `pinned = ∅` in every key, the table is a traversal
over values `tu(TU)` needs anyway for emission and `odr_report`, no new question cells exist, and
no body depends on another body. In the divergent case, one re-analysis of the divergent item per
round under a TU-specific key (the pin's context is this TU's), with the pinned instantiation
itself not recomputed; the result for `J` is then exactly the sequential compiler's. The
`is_complete<T>` idiom and its relatives are the programs that take this path. A note-level
diagnostic ("`X<S>` at a.cpp:14 uses the instantiation from a.cpp:10, where `S` was incomplete")
is available as a warning and off by default.

**Variable templates, static data members and satisfaction** are not pinned. Their
specializations have the end of the TU as an additional point of instantiation, so two points
that disagree make the program ill-formed, no diagnostic required ([temp.point]/7); identical
atomic constraints with different satisfaction results at different points are likewise IFNDR
([temp.constr.atomic]/3). For these the table entry with a different fingerprint is reported as
an error with the differing question found by the §B6.6 technique ("`V<S>` evaluated at a.cpp:14
differs from a.cpp:10; `S` is complete at the second point and not at the first"). Pinning them
would silently pick one meaning for a program the draft says has none.

The example the design must get right:

```cpp
// lib.h
namespace lib { struct S {}; void f(S, int); }
template<class T> void call(T t) { f(t, 0L); }     // f is dependent
// a.cpp
#include "lib.h"
void use(lib::S s) { call(s); }
// edit: append   namespace lib { void f(S, long); }   at the end of a.cpp
```

`instantiate_fn(call<lib::S>)` in `a.cpp` recorded `visible_decls(lib, f, at END) = {f(S,int)}`
plus the stored definition-context set (∅ for `f` at the template). The edit changes that answer;
`a.cpp`'s END prefix is new, its walk leaves the stored path at that question, a new leaf
computes, `f(S, long)` wins, the IR of `call<lib::S>` in `a.cpp` changes. `b.cpp`, which includes
`lib.h` without the extra overload, still walks to the old leaf. The two TUs select different
fingerprints for one index, so `odr_report` says that `call<lib::S>` means different things in
`a.cpp` and `b.cpp`, which is the ODR/IFNDR situation the user probably did not intend.

#### B6.3 Specialization selection

`select_specialization(primary, args, prefix)` gathers `explicit_specs(primary, prefix)` and
`partial_specs(primary, prefix)`, both question cells over the syntactic indexes,
matches each candidate with `match_partial(spec, args)` ([temp.spec.partial.match]; pure, shared)
and orders with `more_specialized(a, b)` ([temp.spec.partial.order]; pure, shared). **The prefix
is the instantiating computation's own**: an instantiation at prefix `P` selects among the
specializations visible at `P`, which is what a sequential compiler does and is the only prefix
the computation may ask at (§B1.4); an instantiation demanded by `tu(TU)` at END selects among
the TU-wide set. A partial or explicit specialization must be reachable from every use that
would select it, in every TU, or the program is ill-formed, no diagnostic required
([temp.spec.partial.general]/1, [temp.expl.spec]/7), so a declaration that appears after such a
use can only make the program IFNDR, and we diagnose it: `tu(TU)` fetches, for every class and
variable specialization in its first-demand table (§B6.2), `select_specialization(primary, args,
END)`, a cheap cell (question cells plus memoized matches), and compares the winner with the one
recorded in the instantiation's result. A different winner is "specialization of `X` declared
at a.cpp:30 after a use at a.cpp:12 that it would affect". Sharing is unaffected in practice:
specializations live in headers, so the answers to `explicit_specs`/`partial_specs` are the same
at every demanding prefix and the walks land on one leaf.

Across TUs:

```cpp
// a.cpp: #include "x.h"  X<int> a;            // primary
// b.cpp: #include "x.h"  template<> struct X<int> {...}; X<int> b;
```

`instantiate_class(X<int>)` has one leaf from `a.cpp` whose path says `explicit_specs(X) = ∅`;
`b.cpp`'s walk leaves the path at that question, computes a second leaf, the two TUs select
different fingerprints, and `odr_report` reports the conflict with both locations. Adding a partial
specialization later re-runs `select_specialization` for every specialization of that primary in
that TU; each is a few memoized matches, and the winner is usually unchanged, so cutoff.

#### B6.4 Points of instantiation and ordering

Which instantiations a TU emits is the transitive closure of the demand lists (§B1.4) reached from
the TU's items, computed by `tu(TU)`. **Emission order is by mangled name** (adopted by §A5.3):
the mangled name is a function of the entity id, so moving a use within the TU does not reorder
the object file, and two TUs emitting the same COMDAT group emit it identically. Source positions
are used only for attribution: diagnostics from an instantiation are attached to the first
demanding `Loc` in the TU's first-demand table (§B6.2), and the "required from here" chain is the
path of request positions through the demand lists. Nothing is ordered by completion order
(DECISIONS §4.4).

#### B6.5 Constraints and `requires`

`satisfies(concept entity, canonical args, ctx)` normalizes ([temp.constr.normal]) and evaluates
atomic constraints with `atomic_satisfied(atomic id, mapping, ctx)`, each a substitution followed by
constant evaluation (§B7); `requires`-expressions ([expr.prim.req]) substitute each requirement and
record the lookups, conversions and completeness checks they perform, with substitution failure
as a `false` *value*. The prefix is the demanding site's, not END: a requirement like `requires {
sizeof(T); }` is legitimately false before `T` is defined, and a program that checks it only there
is well-formed. If identical atomic constraints with the same arguments have different
satisfaction results at different points in the program, the program is ill-formed, no diagnostic
required ([temp.constr.atomic]/3); we diagnose it exactly as for class templates (§B6.2): the
index `(atomic constraint, mapping)` appears in the demand lists, `tu(TU)`'s first-demand table
sees two fingerprints for it within a TU, and `odr_report` sees them across TUs. Memoization by
the atomic constraint and its arguments is the caching the draft describes; a well-formed program
walks to one leaf from every point.

#### B6.6 The re-verification technique

Several rules say "if this gives a different answer at another point, the program is ill-formed,
no diagnostic required" ([temp.point]/7, [temp.constr.atomic]/3, [temp.spec.partial.general]/1,
[temp.expl.spec]/7, [class.member.lookup]/6). Each is implemented by one mechanism: the same
index fetched at two prefixes, and the selected fingerprints compared. The two cells come from
two demanding sites (the first-demand table of §B6.2), from a site and END (`tu(TU)` for function
templates and specialization selection), or from a member declaration and the complete class
(`class_def`). When the program is well-formed the second fetch is a trie walk, one hash lookup
per trace element and no computation; only when an answer differs is a second result computed,
which is exactly the case being diagnosed. The explanation is the first question on the selected
path whose answer differs at the other prefix ("`f(S, long)` is visible at the end of the TU but
not at the point of instantiation"), computed from live question cells. Divergence between TUs is
the same comparison performed by `odr_report` over the fingerprints each `tu(TU)` selected.

### B7. Constant evaluation and reflection

Constant evaluation runs ycxx's IR in the interpreter (DECISIONS §4.6) inside whichever query
needs the value (an initializer, a template argument, `static_assert`, an array bound,
`if constexpr`, a satisfaction check). It is not itself a query per expression: the evaluation of
one expression, with all the constexpr calls it makes, runs inside the query that contains the
expression (`decl_sema`, `analyze_body`, `instantiate_*`, `atomic_satisfied`), with a step limit,
so constexpr recursion is a step-limit error and never a cycle (§A2.4). The memoized units are:

    constant_value(variable entity, prefix, evaluating = ∅)   the value of a variable usable in
                                                              constant expressions, from its
                                                              initializer at the variable's own prefix
    function_ir(function entity, prefix)                      the IR the interpreter runs

`evaluating` is the ordered set of variable entities whose initializers are being evaluated on the
current chain (the "in progress" flag of a sequential compiler made a key argument, §A2.4). A
read of a variable in `evaluating` yields "not usable in constant expressions" ([expr.const])
without a fetch; otherwise the read fetches `constant_value(V, prefix, evaluating ∪ {current})`.
The same set is threaded through `instantiate_fn` and `instantiate_var` when a constant
evaluation demands them (§B6.2), because an instantiated body can contain a constant expression
that reads a variable on the chain (`template<class T> constexpr int f() { int a[V<T>]; … }`
with `template<class T> constexpr int V = f<T>();`). It is empty in every ordinary demand, so the
key space grows only along such chains, which exist only in ill-formed programs. `pinned`
(§B6.2) travels in the same context and is never changed by an evaluation. The entity in
the key is enough for variable template specializations (`V<int>`) and static data members of
class template specializations (`X<int>::n`); their initializer is instantiated by the
`constant_value` computation itself. What an evaluation records:

- `function_ir(F, ctx)` for every call, and `definition_before(F, at I)`: a constexpr function
  that is not defined at the point of evaluation makes the expression non-constant
  ([expr.const]); the negative answer changes when a definition appears before `I` and stays
  equal when one appears after.
- `constant_value(V, ctx, evaluating)` for every `constexpr`/`constinit`/const-integral variable
  read, and `definition_before(V, at I)`.
- `layout(C, ctx)` for `sizeof`, member offsets and object representation, `vtable(C)` for virtual
  calls, `class_def(C, ctx)` for member access, `complete_before(C, at I)`.
- For template specializations needed by the evaluation, `instantiate_fn(spec, ctx, evaluating)`
  at this point's prefix (§B6.2).

Reflection ([meta.reflection]): `members_of(^^C, ctx)` on a class fetches `class_def(C, ctx)` and
is therefore as coarse as the member list, which is the right granularity because the result
*is* that list. `members_of(^^N)` on a namespace enumerates members visible at the point of
evaluation and records `namespace_members_before(N, at I)`, a deliberately coarse query (any
addition to `N` before `I` invalidates it; again the result enumerates them, so nothing finer is
possible). `type_of`, `parameters_of`, `template_arguments_of` and friends map to
`decl_sema`, `function_type`, and the canonical type, all already recorded.

`define_aggregate(^^S, …)` and the declarations produced by a `consteval {}` block
([dcl.pre]) are side effects, modelled as results: `item_effects(J, ctx)` is the list of
synthesized declarations an item's plainly constant-evaluated evaluations produce, with ids
derived from `(J, ordinal)`. `definition_item(S, ctx)` consults the region indexes *and*
`injected_definitions(prefix)`, a question cell aggregating `item_effects` over the items of the
prefix, kept per item list so that a region with no reflection produces an empty value that cuts
off. The draft
restricts injection to plainly constant-evaluated contexts, which is what makes `item_effects` a
function of the item rather than of the order in which speculative evaluations happened to run.

Determinism: the interpreter has no clock, no addresses that escape, and deterministic iteration
orders; `__COUNTER__`-like state does not exist at this level.

### B8. Class completeness and layout

- `definition_item(E, prefix)`: which item (or injected declaration) in the prefix defines `E`.
- `complete_before(C, prefix)`: a question cell, true iff `definition_item(C, prefix)` exists (for
  a class template specialization: iff its instantiation has been demanded, which the demanding
  computation knows because it demands it) or the prefix is inside a complete-class context of
  `C`. It is a cell, never a transparent function, because its answer is what traces branch on
  (adopted by §A2.4 and §A10 in place of `is_complete_at(S, pos)`). Its change invalidates exactly
  the observers of (in)completeness: `sizeof` in a `requires`, derived-to-base conversions,
  member access, `C::m`, base specifiers, ADL association by bases, `is_complete` tricks.
- `class_def(C, ctx)`: the analysed definition without its complete-class contexts: bases
  (resolved), members with their types, access, `virtual`/`override`, friends, nested types,
  static data members. Default member initializers, member function bodies, default arguments and
  noexcept-specifiers are separate queries (`default_member_init(m, ctx)`, `analyze_body(m, ctx)`,
  `default_arg(p, ctx)`, `noexcept_spec(f, ctx)`), analysed in the complete-class context
  ([class.mem.general]) and only when demanded, which also matches [temp.inst] for class template
  members.
- `layout(C, ctx)`: sizes, alignment, field offsets, base offsets, vptr positions; depends on
  `class_def(C)`, `layout` of bases and member types, and the target ABI configuration. Keyed by
  entity id, so TUs share it; divergence is an ODR diagnostic.
- `vtable(C)`, `vtt(C)`: from `class_def` and `layout`; also entity-keyed.

Why `class_def` as one value is acceptable: a member added to `C` changes the type of `C` for every
user in the sense that matters (layout, lookup results that fall through to bases, conversion sets)
and the per-name filters (`class_decls_named`) sit on top of it as cheap derived queries that cut
off. A finer split (members indexed by name as separate queries) would multiply entries for little
gain, because a class is edited as a whole far more often than a namespace is.

### B9. Preprocessor

`pp(file identity, prefix)` is a contextual query. Its prefix is the token-level refinement of
§B1.1: at the leaf level the including region's tokens up to the directive rather than its items,
since items are not known until the region is complete (an `#include` inside an item is placed
before that item for the item-level prefix). Macro state, the `#pragma once` set and the
`__COUNTER__` value are functions of the preceding tokens, so each is a question at the prefix
(`macro(name, prefix)` over per-prefix-node persistent maps that extend the parent's by the
region's delta). Its result is the region: tokens
(with include references), the macro delta, the include list, an include-guard summary
(`guarded_by(X)` when the file is one `#ifndef X … #endif`), and diagnostics. What it records:

- `macro(name)` for every identifier it tested, with the answer "undefined" or the definition's
  tokens; local `#define`s shadow and are not recorded. This is the subset of macro state the
  file depends on: typically thousands of negative answers and dozens of positives, verified in
  microseconds against a persistent hash map ([cpp.replace], [cpp.cond]).
- `include_resolution(spelling, kind, includer directory)` for each `#include` and
  `__has_include`, which depends on `file_exists(path)` inputs along the search path, negatives
  included, so that creating a header that shadows one later in the path invalidates the right
  regions ([cpp.include]).
- `once_included(file)` for each include attempted; `counter_at_entry` only if `__COUNTER__` is
  used; `__has_cpp_attribute`, `__has_builtin`-style probes and predefined macros are configuration
  inputs ([cpp.predefined]).

`__LINE__` and `__FILE__` are functions of the file's own content and spelled path. `__DATE__` and
`__TIME__` are an input `build_time` (§A1.1) set by the client per build request (default: the
request time); a region that uses them records it and is legitimately re-preprocessed every
build, with a warning. Incremental equals cold because the cold run is given the same `build_time`.

Reuse: a header's region is reused across include sites and TUs whenever its trace verifies. A
guarded header included a second time verifies the trace `{macro(X): undefined}`, fails, and
computes a tiny entry `{macro(X): defined} → empty`, after which both entries serve every future
inclusion. Within a TU the same file with the same answers is the same region: the tree is one
object however many include items bring it in.

### B10. Textual inclusion, modules, ODR

A header's meaning depends on what precedes it: macros (handled by `pp`'s trace) and
declarations (handled by `visible_decls` answers in the traces of its items' `decl_sema`,
`analyze_body`, `class_def`). Both are contextual queries, so a header analysed in one TU is
reused in another iff every question it asked of the preamble has the same answer there. TUs
whose prefixes at the header are the same handle do not even walk twice: they share the selection
cell (§B1.1). Nothing is assumed about preamble equality; a TU that defines a macro the header
tests simply walks to, or creates, another leaf.

Named modules ([module.unit], [module.import]): a module interface unit is a TU whose exported
declarations form `module_interface(M)`, a *non-contextual* result (the importer's preamble cannot
influence a module unit). Importing fetches `imported_decls_named(M, S, name)`, filtered by
visibility and reachability ([module.interface], [module.reach]); the global module fragment is
pruned by decl-reachability ([module.global.frag]). Header units ([module.import]/5) are `pp` runs
in an isolated context (command-line macros only) whose macro delta is imported; the region is
then shared like any other.

ODR ([basic.def.odr]) as a by-product: every entity-keyed query (`class_def`, `layout`,
`function_ir` of an inline function, `instantiate_*`, `satisfies`, `constant_value` of an inline
variable) can yield two different results for one index only when two TUs saw different
definitions or different environments. `tu(TU)` emits, as part of its value, the ordered list of
entity-keyed indexes it demanded with the fingerprint it selected for each; `odr_report(program)`
merges these lists in command-line order and reports every index with more than one distinct
fingerprint, ordered by index and then by the TUs involved. The explanation is computed from live
cells by the §B6.6 technique: the first question on one TU's selected path whose answer differs at
the other TU's prefix ("`inline int f()` is defined by `x.h:12` in `a.cpp` and by `y.h:40` in
`b.cpp`"). Nothing is read from the trie, so stale leaves and collection timing cannot change the
report. Multiple non-inline definitions are reported by our linker from the symbol table, as today.

### B11. Granularity and walkthroughs

Query units, from coarse to fine: `tu(TU)` (object file: aggregates symbols and diagnostics in
source order); `pp`/trees/regions; `region_items`, `item_tokens`, `body_tokens`, the syntactic
indexes; `decl_sema(item, ctx)` (type, entity, linkage, attributes, default arguments,
constraints of one declaration); `class_def`, `layout`, `vtable`; `analyze_body(item, ctx)` →
`function_ir(entity, ctx)`; `default_member_init`, `default_arg`, `noexcept_spec`;
`instantiate_*`, `select_specialization`, `deduce`, `satisfies`; `resolve`, `implicit_conversion`;
`constant_value`; and the question queries `visible_decls`, `member_lookup`, `adl`,
`directive_closure`, `complete_before`, `definition_item`, `macro`.

**(i) Editing a function body in `a.cpp`.** The main region's hash changes; `region_items` re-runs
(a lex and split of the file, milliseconds); item ids are unchanged, and `body_tokens(f)` changes.
Items before `f` keep their prefixes and nothing about them is touched. Items after `f` have new
prefixes (their item-list handle now contains the new body); their selection cells walk the trie,
each question answered in O(1) from the new indexes, and land on their old leaves.
`analyze_body(f)` re-runs, `function_ir(f)` re-runs; if `f` is
`constexpr` and was evaluated elsewhere, those evaluations re-validate on `function_ir(f)` and cut
off if the IR is equal. If `f` has a deduced return type, `return_type(f)` re-runs and callers cut
off when it is unchanged. Code generation for `f` only.

**(ii) Adding `void f(long)` to `N` in a header included by 500 TUs.** The header's region and tree
hashes change; `region_items` and the region indexes recompute once (shared). Item ids in the
header are unchanged. Every item after the insertion, in the header and in everything included
after it, has a new prefix and therefore a new selection cell, which walks the trie: items that
asked `visible_decls(N, f, …)` leave the path at that question and the bodies that call `N::f`
re-run (their `resolve` fetch has a new candidate set, computed once per distinct call shape
across all TUs); bodies whose winner is unchanged produce equal results and cut off. Every other
walk lands on its old leaf. Walks are per distinct prefix, not per TU: TUs with the same include
order up to the header share the new cells. Expected cost: per distinct prefix, one O(1) question
per trace element; per calling body, one re-analysis. Nothing is re-parsed except the header once.

**(iii) Adding a partial specialization of `X`.** `region_specializations` changes;
`select_specialization(X, args, ctx)` re-runs for each specialization of `X` demanded in TUs that
see the new declaration (`match_partial` against the new candidate, memoized across TUs); winners
unchanged cut off; a changed winner re-instantiates that one class and its dependents. The
END comparison in `tu(TU)` (§B6.3) reports uses that preceded the new specialization and would
have selected it.

**(iv) Adding a hidden friend to `S`.** `class_def(S)` changes (new item in the class body).
`friend_decls(S, name)` changes for that name only; ADL lookups with `S` associated that asked that
name re-run; `layout(S)` re-runs and cuts off (friends do not affect layout); `class_decls_named`
for other names cut off.

**(v) Adding `#define FOO` to a header `h.h`.** `pp(h.h)` recomputes; its delta now defines `FOO`.
Every later region in each TU that tested `FOO` (`#ifdef FOO`, or merely spelled the identifier
`FOO`) fails its `macro(FOO)` question and re-preprocesses; regions that never mention `FOO`
verify. A re-preprocessed region whose complete result (tokens, macro delta, `#pragma once` and
`__COUNTER__` effects) is unchanged keeps its tree id, so the prefixes of what follows it are
unchanged and nothing below it moves; `h.h` itself has a new tree id, so everything after its
include item in each TU gets a new prefix and walks the trie, landing on old leaves wherever
`FOO` was not asked about.

### B12. Soundness and test strategy

The discipline: a semantic computation may learn about anything outside its own item only through
the query context handle. Enforcement:

1. **Type discipline.** AST nodes refer to other items by declaration or entity id; there is no
   pointer from one item's AST to another's. The only object that can answer a question about
   another item is `QueryCx`, every method of which records. Mutable global tables do not exist
   in the semantic layer; even the canonical type interner is per revision and hands out values
   compared structurally.
2. **Review rule (P0).** A commit that reads a declaration, a macro, a file, a layout or a
   constant value other than through a recorded query is rejected at review.
3. **Forced re-execution mode** (`--verify-memo`, §A9). The engine validates
   normally *and* re-executes every green query, comparing results; for a selection cell it also
   compares the fresh trace with the stored path element by element, so a missed dependency and a
   nondeterministic question order are both named. It runs on every suite in CI.
4. **Incremental-equals-cold** (DECISIONS §4.2, §5.4) over generated edit sequences: insert,
   delete, move and rename declarations, toggle macros, reorder includes, add specializations and
   friends; after each step compare all results and diagnostics with a cold run. The generator is
   biased toward the cases in this document.
5. **Perturbation oracle.** For a result with recorded dependencies `D`, mutate an input outside
   the transitive closure of `D` and assert the result is unchanged. A change means an unrecorded
   read.

Over-invalidation accepted on purpose, with expected cost:

- Body-level cutoff: an overload change re-analyses every body that calls the name, even when the
  winner does not change (microseconds each).
- `class_def(C)` as one value; `conversion_functions(C)`, `members_of`, `namespace_members_before`
  as whole lists (inherent to what they answer).
- `directive_closure` and `inline_members` per item, not per name.
- Region re-split on any edit to the file (a lex pass).
- Ordinal shift among identical redeclarations when an earlier twin is removed.
- A trace re-ask per recorded question for every item after an edit in the same file (O(1) each).
- `__COUNTER__` and `build_time` users recompute every time they legitimately must.
- An item whose class template demands diverge from the TU's first demand is analysed once more
  per divergent specialization, under a TU-specific pinned key (§B6.2).

### B13. Rejected alternatives, collected

- **Absolute positions in keys.** Shift on every edit above.
- **Whole scope member list as the dependency.** Every addition to `std` invalidates every lookup
  in `std`.
- **Order-maintenance labels as inputs.** Impure, TU-specific, leak into results and block
  cross-TU sharing.
- **Preamble-hash sharing (PCH style).** Shares only among identical preambles; traces share
  among preambles that agree on what was actually asked.
- **Per-expression queries.** Memory proportional to expressions, for a cutoff that body-level
  already gives at negligible cost.
- **Instantiating at every point of instantiation.** Multiplies work; the IFNDR rule lets us
  instantiate once and re-verify a trace.
- **Context in the instantiation key.** The context is unbounded and unknowable before the
  computation; the trace is exactly the part of it that mattered.
- **`(TU, position)` as the context, with a divergence hook on the memo table.** Part B's
  first proposal; replaced by prefixes and `odr_report` (§A1.9): TU-keyed cells
  multiply by the TU count, and a hook's output depends on garbage-collection timing.
- **Per-TU prefix chains over all nested regions.** O(regions × names) nodes per TU; the include
  tree aggregation shares everything below the top level.
- **A `first_demand(spec, prefix)` question** for class template instantiation. Its answer is an
  aggregate of the semantic results of every earlier item, bodies included, so every item would
  depend on every earlier body: the TU becomes sequential (§B6.2).
- **Rejecting programs whose class template instantiation depends on demand order** (Part B's
  first answer to the first-demand problem). A conforming program ([temp.point]/7 gives the
  specialization one point of instantiation and later uses see it) would be refused; pinning
  (§B6.2) gives the sequential meaning at a cost confined to the divergent item.
- **Pinning by fingerprint** rather than by the first demand's cell. Needs a fingerprint-to-value
  lookup in the trie and a way to recompute an evicted value without knowing its context; a pin
  that names the cell has neither problem.
- **Pinning variable templates, static data members and satisfaction.** Their divergence is IFNDR
  ([temp.point]/7, [temp.constr.atomic]/3); pinning would hide it.
- **Emission in first-demand source order.** Moving a use reorders the object file and makes
  COMDAT contents TU-dependent; mangled-name order has neither problem (§B6.4).
- **TU-level reanalysis when a header changes.** The status quo this project exists to replace.

### B14. Points settled with Part A

- Contexts are prefixes; traces are complete dependency lists stored as a trie per index;
  divergence is `odr_report` over the fingerprints each `tu(TU)` selected (§A1.9).
- Question cells are recomputed in O(1) from the syntactic indexes and are not persisted; trie
  leaves are persisted as `(index, path, result)` records (§A1.9, §A7).
- Decisions Part B makes that Part A adopts: a tree id is the fingerprint of the complete `pp`
  results (§B1.1); a trace names question cells and stable-id context-free cells, never a
  syntactic index (§B1.3); `Loc = (item id, offset)` rendered and ordered by `tu(TU)` (§B2);
  selection-cell values carry their direct demand list with request positions (§B1.4), which is
  where "required from here" chains come from; `complete_before(C, prefix)` is a cell (§B8);
  the context of a contextual query is `Ctx = (prefix, pinned, evaluating)`, the prefix abstracted
  by the trie and the two maps ordinary key data, empty in ordinary demands (§B1.4, §B6.2, §B7);
  `tu(TU)` builds the first-demand table and repairs class template divergence by re-fetching the
  divergent item with `pinned` (§B6.2); emission order by mangled name (§B6.4); the input is named
  `build_time` (§B9); internal linkage entity ids are TU-free and made TU-local only at emission
  (§B1.5).
- Still to confirm with the preprocessor design: the token-level prefix for `pp` (§B9) and the
  placement of an `#include` that falls inside an item.

## Part C. The spike

[`spikes/query-engine/`](../../spikes/query-engine/) is a throwaway prototype of the engine core
and of the firewall pattern, applied to a toy language with C++-like overloading (`int`, `float`,
`bool`, `include`, exact < promotion < conversion, [over.match.best], ambiguity). It builds
standalone (`make check`) because the build skeleton (#4) does not exist yet. Measured on an
18-core Apple M-series machine with Apple Clang 21:

- **(a) Editing a function body** re-executes `CHECK` and `LOWER` of that function only. The
  file's parse and its per-item projections re-run and cut off. Inserting lines above every
  function re-checks nothing, because body positions are relative to their item.
- **(b) Adding `f(float)` to a header** re-runs exactly the three resolutions keyed on `f`, in two
  TUs. One of them changes its winner, and one body is re-checked. Adding a declaration of `k`
  flips a negative lookup and re-checks only the function that called the missing `k`. Adding an
  unrelated declaration re-runs no overload set, resolution or body.
- **(c) Determinism.** A 2,000-function program is built at 1, 2, 4, 8 and 16 threads, cold and
  then after each of 10 random edits. At every step the output, the set of executed queries and
  cutoffs, and the memo graph (revisions, dependency lists, execution counts) are identical.
  Build time is 0.77 s at 1 thread and 0.05 s at 16.
- **Incremental == cold.** The fuzzer applies 1,000 random edits: bodies changed, declarations
  added and removed, includes toggled, headers deleted and restored, positions shifted, functions
  added and removed. After every edit, the incremental results at 1 and 8 threads equal a cold
  build, and both incremental engines executed the same queries. The same fuzzer was run with one
  dependency deliberately left unrecorded, and it failed at the eighth edit as incremental != cold.
- ASan, UBSan and TSan builds run clean. An independent review of the spike found no P0 issues.
  Its one P1 was fixed: the thread-count comparison originally covered only cold builds, where
  cutoffs are equal by construction.

The spike confirms the parts of Part A it implements:
- ordered dependency groups, validated group by group with every member of a group verified;
- per-entry claiming, so each query executes at most once per revision;
- early cutoff by equality;
- per-name firewall queries, with negative results as values;
- positions relative to items.

It deliberately leaves out several things Part A specifies:
- **Cycles.** Detection walks only the requesting chain, so a cycle between parallel siblings
  would deadlock. The toy language has no cycles. Part A replaces this with a wait-for graph
  (§A2.2).
- **Durability levels** (§A1.4). Every edit walks every reached dependency.
- **Fingerprints.** Cutoff compares values, so values can never be evicted or persisted (§A1.5).
- **Cancellation.** Inputs change only between requests. That is the right rule (§A4.1), but the
  spike has no way to end a request early.
- **Diagnostics as values.** They are values in the toy, but collected ad hoc rather than by
  `diags_of` (§A1.7).
- **Cell storage.** Entries are heap objects; Part A uses dense cell ids in arenas (§A3.6).
- **Scheduling.** The spike uses threads that block in a fork-join, where a joiner runs only its
  own join's tasks. Part A replaces this with coroutines on `std::execution`, so no thread
  blocks (§A3).
- **The C++-specific parts:** contextual queries, items cut from tokens, point-of-declaration
  ordering, scopes, ADL, templates and the preprocessor.

## Part D. Constraints on other designs

These are posted as comments on #2 and #3.

**On the AST, entity and type design (#2):**

- **Items.** Items are the unit of parsing. A token-level splitter runs before the parser, and an
  item's declaration part and body are separable at the token level. Item ids exclude bodies and
  positions (§B1.2).
- **Positions** are `(item, offset)`. File and line are computed by a late query, only for
  diagnostics and debug info. Nothing stored in a long-lived value holds an absolute offset
  (§B2).
- **Two identities.** Declaration ids (syntactic) and entity ids (structural, program-wide) are
  the only references between items. There are no pointers between items' trees and no parent
  pointers: a shared green tree is navigated through a red view that is never stored (§B1.2, §B1.5).
- **Nodes** are immutable, hash-consed, and carry Merkle fingerprints. Types have structural
  equality, and their canonical form does not depend on interning order. Every value is
  equatable, fingerprintable and serializable without the interner.
- **Results are values.** Lookup results are ordered lists of declaration ids with flags. Not
  found, ambiguity, substitution failure, unsatisfied constraints, a non-constant evaluation and
  non-viability are all values with their explanations. `cx.diag` is only for errors that hold
  in every context reaching the cell (§A1.7).
- **"In progress" state** is a position comparison or an explicit key argument. No query demands
  the completion or layout of the class whose member declarations it is analysing (§A2.4).
  Class completion does not depend on member function bodies, default arguments, default member
  initializers or noexcept-specifiers; those are separate queries in complete-class contexts
  (§B8).
- **Templates.** A template's analysed form stores, for each dependent unqualified call, the
  definition-context lookup set as a value, and keeps dependent constructs symbolic (§B6.1).
- **Contextual results.** The context of a contextual query is a `Prefix`, never a TU. The
  results of contextual queries mention nothing context-specific: no TU, no prefix (§A1.9).
- **Demands.** Instantiations, satisfaction checks and constant evaluations are `demanded` kinds,
  fetched with `cx.demand(key, loc)`. The request `Loc` goes into the demanding cell's demand list,
  a value persisted with it, never into the key and never onto an edge; `tu(TU)` builds the
  first-demand table and the "required from here" chains from demand lists (§A1.7, §B1.4, §B6.2).
- **Synthesized declarations** get ids from their origin and an ordinal. These are implicit
  special members, deduction guides, injected-class-names, and declarations created by
  `define_aggregate` or consteval blocks (§B7).
- **Ordering.** Containers iterate deterministically, and no result depends on hash-table order.
- **Kinds and coroutines.** A query's `run` is a `query_task` coroutine unless the kind is
  `cheap`, in which case it is a synchronous function that may fetch only inputs and other
  `cheap` kinds (§A3.3). The question queries of Part B (`visible_decls`, `macro`, the region
  indexes, `complete_before`) are `cheap`; everything that can instantiate, evaluate or analyse a
  body is a coroutine. Keys are structural aggregates of handles (§A10.1); every `Prefix` key
  member that is the computation's context carries `[[=context]]`, on contextual kinds and
  question cells alike; `complete_before(C, prefix)` is a cell everywhere, never transparent.

**On the optimization architecture (#3):**

- **The optimizer as a query.** `optimized(ir_fp, target, options, optimizer_version, level)` is
  a pure memoized query. Budget levels are counted in deterministic units, and all randomness is
  seeded from the key fingerprint (§A8.1).
- **IR hashing.** IR is generated per function from its body alone. It references other entities
  by entity id, and the hashed part carries no source offsets, with debug locations kept in a
  side table. Moving a function therefore changes neither its IR hash nor its optimization-cache
  key.
- **Layout and vtables** are inputs to IR generation through the `layout` and `vtable` queries,
  never recomputed inside the IR generator.
- **Fixpoints.** Interprocedural fixpoints are queries keyed by a call-graph SCC that iterate
  internally; the engine never resolves cycles (§A2.1).
- **Parallelism** inside the optimizer uses positional `par_fetch`/`par_map` or solvers'
  deterministic modes. There is no shared mutable e-graph across tasks.
- **Cancellation.** Solvers poll `cx.stop_token().stop_requested()` at their deterministic
  checkpoints and return the heuristic result when stopped; a solver that runs for more than a
  few milliseconds between checkpoints `co_await`s `schedule(pool)` there to yield its worker.
- **Coroutines.** Optimizer queries are `query_task` coroutines like any other; solver back ends
  behind the constraint layer may be synchronous leaves called from one.
- **Evidence.** Results carry their verification evidence as a value, so a replay re-checks
  instead of trusting.
- **The optimization cache** is read only through `best_available(key, B, S)`, with the snapshot
  `S` an engine input (§A8.3).
- **Order.** Code layout and symbol and section order are functions of IR and cost-model values.
  Cost models are inputs with high durability and a version salt.

## Part E. Open items and follow-up issues

Open items, to be settled in the implementation issues below:

- **Hash function.** Choose the 128-bit hash: a published non-cryptographic design, implemented
  in ycxx with a fixed seed and measured against the fetch-hit budget of about 100 ns.
- **Eviction constants.** The grace periods `k` and `K` and the memory-budget policy, measured on
  libycxx as a corpus.
- **Chaos scheduler.** Its knobs, and whether to record and replay an exact scheduling, not just
  its seed.
- **Medium durability.** The exact policy for `-isystem` paths inside the project tree.
- **Preprocessor prefix.** `pp` needs a token-level prefix (the including region's tokens up to
  the directive), because items are not known until the region is complete. An `#include` that
  falls inside an item is placed before that item. To be confirmed with the preprocessor work
  (§B9).
- **Coroutine frame sizes.** Measure the frame size of representative queries under GCC 16 and
  the cost of the free-list path against the budget of §A3.3; decide whether large frames need a
  size cap enforced by a `static_assert` on `sizeof` of the promise plus locals.
- **Bootstrap workarounds.** None needed by the probes of §A10.4. Each one that becomes necessary
  is marked `// BOOTSTRAP:` and listed in STATUS (DECISIONS §3.2).
- **libycxx `std::execution` gaps** that touch the daemon's edges: `task_scheduler` allocating its
  backend per construction (construct one and keep it), `spawn_future` reporting no completion
  domain (avoid `spawn_future` in the engine; `spawn` into a `counting_scope` suffices).

Follow-up implementation issues are opened when this design is accepted, one per row:

| Issue | Area | Content |
| --- | --- | --- |
| Engine core | `area:query-engine` | Cells, the sharded table, fingerprints, red/green with groups, durability, diagnostics as values with `diags_of`, the query API of §A10 |
| Scheduler | `area:query-engine` | `pool_scheduler` (Chase–Lev deques, two priorities, `std::execution::scheduler` and `parallel_scheduler_replacement` backend), `query_task`, `cell_sender`, `par_fetch`, frame free lists, wait-for-graph cycle detection over suspended frames, stop-token cancellation |
| Contextual queries | `area:query-engine` | Prefixes, selection cells, per-index trace tries, `odr_report` |
| Memory and persistence | `area:query-engine` | Interners, three-step eviction, the on-disk cache and loading |
| Incremental == cold harness | `area:tests`, `area:query-engine` | Edit generators, table comparison, `--verify-memo`, chaos scheduler, cancellation injection |
| Item splitter | `area:parse` | The token-level pre-pass that cuts items, and item ids |
