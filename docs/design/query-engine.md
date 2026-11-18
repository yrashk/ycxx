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
No compiler's source code or design documents were consulted. The issue's prior-art list also
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
- **Review rounds.** The Opus session reviewed both drafts and sent three rounds of pushback. Each
  changed the design:
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
   `(index, prefix)` is an ordinary red/green *selection cell*. When it must execute, it first
   walks a per-index trie of recorded traces (*verifying traces*), and reuses any result whose
   questions get the same answers at this prefix. At most one leaf can match, so selection never
   depends on history. Results are shared across TUs exactly where every question they asked
   agrees, not only where preambles are identical. (§A1.9, §B1.4)
3. **Identity without positions.** The preprocessed token stream is cut into *items* before
   parsing. An item's id comes from its scope path, its declaration-part tokens and an ordinal
   among identical siblings, never from where it sits or from its body. Positions are
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
   declaration set, argument shapes and kind of call. Identical call shapes share one entry
   program-wide. A new overload re-analyses only bodies that looked up its name, and only bodies
   whose chosen candidate changed propagate further. (§B5)
6. **Templates.**
   - Function template definitions are instantiated in the end-of-TU context.
   - Class and variable templates, and concept satisfaction, are instantiated at first demand.
   - Specialization selection uses the TU-wide set.

   The "must mean the same at every point" rules, which are ill-formed with no diagnostic
   required, are *diagnosed*: a selected trace is re-verified at the other point. Divergence
   across TUs comes out of `odr_report`. (§B6, §B10)
7. **Cycles are internal errors, never recovered.** Every "in progress" situation in C++
   (completeness, deduced return types, mutually referential constant initializers) is a
   position comparison or an explicit key argument. Cycles are detected on a wait-for graph and
   reported deterministically. (§A2)
8. **Concurrency.** ycxx has its own fork-join pool on Chase–Lev deques with interactive and
   background priorities. A joining worker runs only its own join's tasks, and blocking is
   compensated by spare threads. `std::execution` appears only at the daemon's edges. (§A3)
9. **Cancellation is cooperative, and revisions advance only at quiescence.** Every completed
   cell survives a cancelled request; nothing half-computed is ever stored. (§A4)
10. **Determinism.**
    - Every order that reaches an output comes from stable keys.
    - Parallel results are positional.
    - Diagnostics are values: only errors that hold in every context are emitted as
      diagnostics, and they are sorted and deduplicated after the fact.
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

Two flavours of derived query, chosen per kind by the dependency design:

- **Memoized** (the default): has an entry in the memo table, its own dependency list, its own
  value and fingerprint. This is what gives early cutoff.
- **Transparent**: not memoized. Its fetches are recorded into the *caller's* dependency list, as
  if the caller had made them. Use it for helpers that are called once per caller and would only
  bloat the table. A transparent query cannot act as a firewall; a memoized one can.
- **Contextual** (§A1.9): memoized, with a key `(index, prefix)`, whose execution first tries to
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
- **Small**: a key should fit in 32 bytes. `decls_named(scope, name)` is two 32-bit handles. Large
  arguments (a token sequence, a macro environment) are interned first and passed by handle.

Negative results are ordinary values. `decls_named` returning an empty list is an entry like any
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

Dependencies are stored as an ordered list of groups, each group an ordered list of cell ids (§A3.3).
Program order, not completion order (§A3.4). The validation algorithm, per entry:

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
read the value. Because an entry's storage is never reused within a revision (§A3.3), there is no
ABA on the claim word.

#### A1.7 Diagnostics are values

A query reports a diagnostic with `cx.diag(...)`. The engine appends it to the executing frame's
list, and the list is stored with the entry as part of its output, fingerprinted with the value.
A diagnostic is a value: `(stable_location, id, args)`, where `stable_location` is
`(item id, offset)` (§B2), turned into a file and line only when rendered. It contains no pointers, no text and no context chain;
rendering, `-W` filtering and context are later, pure steps. Diagnostics are stored unfiltered, so
changing warning flags changes the report without invalidating a single cell.

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
subgraph (the millions of entries with nothing to say cost one bit each). `tu_diagnostics(tu)` is
`diags_of(root cell of the TU)`, deduplicated as §A5.2 says. A header's error appears in each TU
that includes it, which is correct C++, and the build-level report deduplicates across TUs.

**Context ("required from here").** Instantiation keys exclude the demanding TU (Part D), so the
chain of requests cannot live in the instantiation's own stored diagnostics. It lives on the
*edges*: a fetch may carry a context tag, `cx.fetch<Q>(key, cx.at(pos))`, meaning "this cell was
demanded from position `pos` of the fetching cell"; the dependency design uses it for
instantiation requests and for the other places a sequential compiler prints a "required from"
note. `diags_of(parent)` wraps each child's collected diagnostics with `(child key, pos)` when the
edge is tagged, and passes them through unchanged otherwise. Each level adds one frame, so the
chain accumulates on the way up to the TU root while `diags_of(child)` stays context-free and
shared. Tagged edges are rare, so they are kept in a per-cell side list and untagged edges stay
four bytes.

A consequence: an error inside `X<int>` requested from 500 sites in one TU would collect 500 times
with 500 chains. A sequential compiler reports it once, at the first point of instantiation; so
do we, by the deduplication rule in §A5.2, which keeps the lexicographically smallest chain. Fatal
errors are values too: a query that hits one returns a value saying "stopped here", and its
consumers decide what to skip; nothing throws across query boundaries except engine control flow
(§A4.3, §A2.3).

Rejected: a global diagnostic sink. It makes diagnostics an effect, so an entry replayed from
memo would not re-emit them, and ordering would follow completion.

#### A1.8 Firewalls and projections

A projection like `decls_named(scope, name)` built on top of `scope_members(scope)` needs no
special status: it is a memoized query whose re-execution is cheap and whose fingerprint usually
does not change, so the propagation stops at it. The engine offers two hints, both about cost, not
semantics: `cheap` (never spawned as a separate task in `par_fetch`, always run inline, value
evicted last) and `transparent` (§A1.1). The dependency design decides, per kind, which to use. The
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
plus 5×10^7 selection cells across 500 TUs, 15–20 GB at the cell sizes of §A3.3, and 500 trace
verifications per item on every header edit. With prefixes, an item has as many selection cells as
there are distinct include orders leading to it, typically tens, so the same program is on the
order of 10^7 cells, under a gigabyte, and a header edit verifies each item once per distinct
prefix. "Context equivalence by slot-prefix digest" is therefore not a verification shortcut
bolted onto TU-keyed cells; it *is* the key.

**Selection cells.** A contextual query is an ordinary query kind with `Key = (Index, Prefix)` and
the `contextual` trait. The cell for `(index, prefix)` is a **selection cell**: a red/green cell
like any other, whose value is the selected shared result (a handle) and its diagnostics, and
whose dependencies are the selected result's trace instantiated at this prefix. Early cutoff,
durability, cancellation, persistence and the determinism rules apply to it unchanged, because the
engine never sees anything but a cell with dependencies and a value. What is special is only how
it *executes*:

```
execute(sel(index, prefix)):
  node = trie[index].root
  loop:
    if node is a leaf:                          # every question on the path answered as recorded
        record deps = path questions @prefix;  return (node.result, node.diags)
    answers = par_map(node.questions, q -> fp(fetch(q @prefix)))   # one trie node = one dep group
    edge = node.edges[answers]
    if edge missing:
        claim node for computing (others arriving here wait, then re-walk)
        (value, diags, trace) = run(index, prefix)            # ordinary tracked execution
        assert trace starts with the path to node             # determinism detector (debug: ICE)
        trie[index].insert(trace with prefix abstracted, value, diags);  release
        return (value, diags)
    node = edge.target
```

The **trace** is the computation's complete dependency list in program order, groups included:
contextual questions *and* context-free dependencies (region indexes by content hash, pure
matchers, body tokens). Each element is `(key pattern, answer fingerprint)`, where a key pattern is
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
positions, never a TU or a slot; Part D). Its residual risks are the fingerprint collision of §A1.5
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
unchanged (their question cells re-execute in O(1) because the region index keys changed, and cut
off) and gives new prefixes to items at or after `k` in that region and to everything included
after them in the TU. Those items get *new* selection cells, each executing as a trie walk: one
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

Every executing query is a **frame**: `{cell, parent frame, root request, path index in parent's
group, waiting_on}`. A frame can be waiting on a cell in three ways: a blocked fetch on a cell
that another frame is executing or validating; an inline child execution (the parent is waiting
on the child's cell until it returns); and a join on a parallel group whose stolen members are
still running (waiting on each outstanding member's cell). Together with each cell's owner frame,
this is a wait-for graph.

When frame `F` is about to block on cell `c` owned by frame `G`, it takes the engine's single
**wait mutex**, records `F.waiting_on = c`, and walks: `G`, then whatever `G` waits on, and so on.
If the walk reaches `F`, there is a cycle; otherwise `F` parks. The mutex is taken only when a
frame blocks or is woken, which is rare relative to fetches (it happens when two frames want the
same not-yet-computed entry at the same time). Inline-child and join edges are written by the
owning thread with release stores; they are consistent for the walk because a cell's owner does
not change while it is `Running` within a revision, and a finished cell has no owner, which ends
the walk. The old parent-chain walk is the special case where every edge is an inline-child edge.

#### A2.3 Deterministic reporting

On detection, the detecting frame computes the set of cells on the cycle and sorts their keys by
`(kind id, key fingerprint)`. The ICE is `"internal error: query cycle"` with that sorted list,
and, as a note, the program-order path from the detecting frame's root. Every frame on the cycle
is unwound with `CycleUnwind`, their cells reset to `Empty` (nothing is stored), and the exception
propagates to the root request, which fails. Because the sorted set depends only on the cycle and
not on which member was entered or which thread found it, the same cycle always prints the same
message. When a strongly connected component contains several cycles, parallel runs may find
different ones; since this is an ICE in any case, the canonical report is defined as the one a
`--threads=1` run prints (program-order DFS), and the ICE message says so.

#### A2.4 Which C++ situations look like cycles, and how the front end avoids them

The rule for the dependency design: **anything a sequential compiler tracks as an "in progress"
flag is either a position comparison or an explicit key argument.**

- **Class completeness.** `struct S { static constexpr int n = sizeof(S); };` is ill-formed because
  `S` is incomplete at that point; it is not a cycle. Completeness is positional:
  `is_complete_at(S, pos)` is true after the closing brace and inside complete-class contexts
  (member function bodies, default arguments, default member initializers, noexcept-specifiers).
  Analysing a member declaration must never demand `layout(S)` or `complete_class(S)` for the
  enclosing class; it asks `is_complete_at`. For instantiated classes the positions are the
  template's positions plus the instantiation context, so the same rule applies.
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
  Same device: `constant_value(var, evaluating = {...})`; a variable found in its own evaluation
  context is "not usable in constant expressions", per [expr.const]. The context is empty in all
  ordinary evaluations, so the key space does not blow up.
- **Default member initializers used in the class.** DMIs are complete-class contexts, so they are
  evaluated with the class complete; a DMI that uses another DMI uses it through a member
  access on a complete class. Not a cycle. Order-of-initialization questions are positional.
- **Recursive template instantiation** (`X<N>` needs `X<N-1>`) is depth, not a cycle; the depth
  limit is checked by the instantiation query itself. `X<N>` needing `X<N>` through a member
  initializer is caught by the evaluation context above.
- **Constexpr recursion** is evaluated inside one `evaluate(expr, context)` query with a step
  limit; individual calls are not queries, so infinite recursion is a step-limit error, not a
  cycle.

If a corpus program produces an engine cycle, the fix is in the front end, found by the
`--threads=1` report; the test suite (§A9) treats an ICE as a failure like any other.

### A3. Concurrency

#### A3.1 Scheduling: fork-join on a work-stealing pool, blocking with compensation

The pool has `P` workers (hardware threads by default). Each worker owns one Chase–Lev deque per
priority level (two levels: `interactive`, `background`). `par_fetch` pushes its members onto the
executing worker's deque at the frame's priority, runs the first member inline, then joins: it pops
and runs members from the top of its own deque while the top task belongs to this join, and when
its own members have all been stolen, it **blocks** until they finish. It never pops a task from an
outer join (that task could block on something beneath it on this stack, a deadlock the wait-for
graph would report as a false cycle) and never steals while joining. Idle workers steal from the
bottom (oldest task) of a random victim, trying `interactive` deques of all workers before any
`background` deque.

A fetch of a cell that is `Running` or `Validating` under another frame also blocks.

Blocking parks a core. To keep `P` cores busy, the pool compensates: a worker that is about to
park signals the pool, which wakes a spare thread (or creates one, up to a cap of `4P`). A spare
that finds no work goes back to sleep. Above the cap, blocking is uncompensated; that costs
throughput, never correctness.

**Deadlock-freedom.** Every blocked frame waits on cells, and every `Running`/`Validating` cell has
an owner frame on some thread. A thread is parked only while its current frame is blocked. If all
threads were parked, following owner edges from any blocked frame would stay inside the finite set
of blocked frames and therefore revisit one: a cycle, which the wait-for walk detects before
parking and breaks by unwinding. Hence at any time some owner frame's thread is runnable, and
since every query terminates (it is a finite computation over finite inputs), it makes progress.
Tasks pushed to a deque but not yet claimed block nobody: their parent will pop them.
Compensation and priorities are irrelevant to this argument; they only decide how many cores do
the work.

**Rejected alternatives.**
- *Fibers (stackful coroutines per frame).* They would let a blocked frame yield its worker
  without stack nesting. Cost: a hand-written context switch per target, a stack per in-flight
  frame, poor debugger and sanitizer support, thread-local state semantics that C++ libraries do
  not expect. Blocking is rare in this engine (it needs two frames to want the same uncomputed
  entry at the same moment), so fibers would buy little.
- *Helping: a blocked worker runs any available task on top of its stack.* Unsafe as described
  above; the spike's restriction to the current join is the safe subset and is kept.
- *`std::execution` senders as the futures abstraction.* Senders describe asynchronous work as
  composable values and are a fine interface at the daemon's edges (the LSP transport, the file
  watcher). Inside the engine the "future" is just a `Running` memo entry, and the composition
  rule is dependency recording in program order, which senders do not know about. ycxx's pool is
  exposed as a `std::execution::scheduler` for those edges; queries never see senders. libycxx's
  `<thread>`, `<atomic>` and futex-style waits are what the pool is built on.

#### A3.2 Interactive versus background work

A root request carries a priority. LSP requests are `interactive`; builds and the optimizer's
idle work are `background`. Workers prefer `interactive` deques when stealing, and a worker whose
`background` task finishes checks for `interactive` work before continuing its own deque. There
is no preemption inside a task: an `interactive` frame that needs a cell being computed by a
`background` frame waits for it; the background thread finishes that cell at full speed. Task
granularity (a declaration, a function body, an instantiation) keeps that wait in the
milliseconds. Long-running leaves (solvers, §A8) poll a cancellation and yield flag at their
deterministic budget checkpoints.

#### A3.3 Shared memory layout and lifetime

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
them without further synchronization. Waiters park on a parking lot keyed by cell id, so cells
carry no condition variable.

#### A3.4 Deterministic dependency recording

Each frame has a dependency recorder: a vector of cell ids with group markers and a small
"already recorded" set. `fetch` appends one singleton group; `par_fetch(k1..kn)` appends one group
with members in **argument order** before any member runs, so completion order never shows. A key
fetched twice by the same frame is recorded once, at its first position (validation of the first
occurrence covers the later ones). Child frames record into their own lists; a transparent query
records into its caller's. On store, the recorder is compacted into the dependency pool.

Validation of a group in parallel (§A1.5) is itself a `par_fetch` of `ensure_green` tasks, so the
same scheduler serves both.

### A4. Cancellation

#### A4.1 Revisions advance only at quiescence

The daemon coalesces file-system events into a batch. To apply a batch it sets the global cancel
flag, which every `fetch` checks on entry and every parked waiter is woken to check; frames unwind
with the `Cancelled` exception, their `Running`/`Validating` cells revert, and the daemon waits
until no frame is running. Then it writes the inputs, advances `R`, and clears the flag. Latency
to quiescence is bounded by the longest fetch-free stretch of work, which is why solvers and the
code generator poll the flag at their budget checkpoints.

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
revision, because nothing has changed yet. `Cancelled` and `CycleUnwind` are the only exceptions
that cross query boundaries; query code must be exception-safe and must not catch them (a
`catch (...)` in query code is a review P0).

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

1. Every order that reaches an output is derived from stable keys: source positions
   `(item id, offset)`, instantiation contexts, mangled names, the command-line order of
   TUs. Never from completion order, cell ids (allocation order differs between runs), thread ids,
   addresses, or time.
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

The build report is `sort(dedup(concat over TUs in command-line order of tu_diagnostics(tu)))`.
The **primary key** of a diagnostic is `(file path, offset, severity, id, args)`; its context
chain (§A1.7) is not part of it. Deduplication is by primary key, keeping the lexicographically
smallest chain (chains compared frame by frame by position, a shorter chain before a longer one
with the same prefix). The sort key is the primary key followed by the surviving chain. A header
error seen by 500 TUs is one line. An error inside a shared instantiation requested from many
sites is one line, attributed to the lexically first request, which is what a sequential
compiler's "first point of instantiation" produces. Per-file LSP
diagnostics are the same query filtered by file. Error limits are applied after sorting, so
"the first 20 errors" means the first in source order, not in time.

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

A frame gets a bump allocator for scratch that is freed when the frame returns; anything that
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
atomic rename. Each record is `(kind, key fp, value fp, deps as (kind, key fp) list with group
markers, durability, value bytes or absent, diag bytes)`, checksummed. Inputs are recorded with
their content fingerprint. Trie leaves of contextual queries are a second record type
(`index fp, path of (pattern fp, answer fp) groups, result fp, result bytes, diags`), and selection
cells are stored without dependencies (§A1.9). Revisions are not persisted; they are a session
concept.

Keys and dependencies are stored as fingerprints, not handles, so the file does not depend on
interning order, two daemons' caches can be merged, and the optimization cache (§A8) can be shared
between machines.

#### A7.2 Loading

On start, records are loaded with `verified_at = -1`, `changed_at = 0`; the interner is rebuilt by
deserializing values (fingerprint to handle map). The daemon hashes every input file it knows
about (in parallel; this is the cold cost of a restart) and sets `changed_at = 1` for the ones
whose fingerprint differs, bumping `last_changed[durability]` accordingly. The first request then
runs the ordinary red–green walk: a header that did not change is green by the durability
shortcut if nothing of its durability changed, otherwise by deep verification. There is no
separate "cache validation" algorithm; it is the same algorithm with a different starting state.

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
`optimized(key, B)` now and record it into the OC for the future. Rule: the result used is always
the cached one with the highest level, and never one *below* `B`. Two consequences, both intended:
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

```cpp
// A query kind is a struct with Key, Value, traits and run().  Registered by the schema.
struct DeclsNamed {
  struct Key { ScopeId scope; NameId name; };            // 8 bytes; hashed and fingerprinted by content
  using Value = DeclList;                                // interned handle, empty list is a value
  static constexpr QueryTraits traits{ .cheap = true };  // run inline, evict last
  static Value run(Ctx& cx, Key k) {
    const Members& m = cx.fetch<ScopeMembers>(k.scope);  // records the dependency
    return m.lookup(k.name);                             // negative result: empty DeclList
  }
};

struct TuDiagnostics {
  using Key = TuId;  using Value = DiagSet;
  static Value run(Ctx& cx, TuId tu) {
    auto [pp, cfg] = cx.par_fetch(q<Preprocessed>(tu), q<Config>(tu));  // one group, argument order
    auto decls = cx.fetch<TopLevelDecls>(pp);
    auto bodies = cx.par_map<CheckBody>(decls.functions());               // vector in key order
    if (bodies.size() > cfg.limit) cx.diag(Diag::TooManyFunctions, cfg.limit);  // a value, not an effect
    return cx.fetch<DiagsOf>(cx.self());                                   // collect, program order
  }
};

// Transparent helper: not memoized, its fetches land in the caller's dependency list.
struct IsCompleteAt { struct Key { ClassId c; SrcPos at; }; using Value = bool;
  static constexpr QueryTraits traits{ .transparent = true };
  static bool run(Ctx& cx, Key k) { return cx.fetch<ClassExtent>(k.c).complete_at(k.at); }
};

// Inputs are set by the daemon, never fetched from the file system inside a query.
db.set_input<FileText>(path, bytes, Durability::Low);   // only between requests, at quiescence
auto report = db.request<BuildReport>(build, Priority::Background);   // a root; returns a future
```

```cpp
// An instantiation request carries the position it is made from; diags_of wraps along this edge.
auto inst = cx.fetch<InstantiateClass>(X_int, cx.at(use_pos));     // "required from here"
// Context-dependent outcomes are values, not diagnostics: the consumer decides.
if (auto r = cx.fetch<Substitute>({tmpl, args}); r.failed()) { /* SFINAE: discard r.reason() */ }
```

```cpp
// A contextual query: index + prefix. The engine selects a shared result by trie walk first (§A1.9).
struct InstantiateClass {
  struct Key { EntityId spec; Prefix ctx; };          // ctx is the point of first demand's prefix
  using Value = ClassDef;
  static constexpr QueryTraits traits{ .contextual = true };
  static Value run(Ctx& cx, Key k) {
    auto specs = cx.fetch<ExplicitSpecs>({k.spec.primary(), cx.prefix()});   // question @ctx
    auto complete = cx.fetch<CompleteBefore>({k.spec.arg(0), cx.prefix()});  // question @ctx
    auto tmpl = cx.fetch<ClassTemplateDef>(k.spec.primary());                 // context-free, still traced
    ...
  }
};
```

`Ctx` offers exactly: `fetch<Q>(key)`, `fetch<Q>(key, at(pos))`, `par_fetch(q<Q1>(k1), …)`,
`par_map<Q>(range)`, `diag(...)`, `self()`, `prefix()` (the cell's own context, the only prefix a
contextual computation may use), `scratch()` (the frame allocator), and `check_cancelled()` for
long leaves.
There is no way to read an input except `fetch<SomeInput>`. Query code may throw its own
`QueryError` types across its own helpers but never across a query boundary: a query's failure is
a value in its `Value` type. `Cancelled` and `CycleUnwind` pass through untouched.

## Part B. The C++ layer: what is a query, what is its key, what it records

*Drafted by a second Fable subagent, revised after review and reconciled with Part A (see
Provenance).* Part B decides which queries exist, how they are keyed so that an edit disturbs only
the questions whose answer could change, and what each semantic step records. It runs on Part A;
the one capability it needs beyond the salsa model, contextual queries, is specified in §A1.9.

### B0. Part B in one screen

1. **The unit of source identity is the item**: a top-level or member declaration cut from the
   preprocessed token stream before parsing. Its id is derived from its own tokens (declaration
   part only, bodies excluded), its syntactic scope path and an ordinal among identical siblings.
   Not from its position, not from the content of the enclosing file.
2. **A program point is `(item, offset-in-item)`.** Visibility of a declaration at a point is a
   query `visible_decls(S, name, at: item)`, decomposed into a shared part (what precedes the
   item's header occurrence in the include tree) and a local part (what precedes the item inside
   its own region). Absolute positions never appear in a key.
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
   ([temp.point]); class and variable templates, concept satisfaction and anything needed for
   constant evaluation are instantiated at the point of first demand. The "same meaning at every
   point" IFNDR rules are *diagnosed* by fetching the same index at the other point's prefix and
   comparing (a trie walk when the answers agree, a second computation only when they do not),
   and across TUs by `odr_report(program)`, a derived query over the results each TU selected.
7. **The region index is syntactic.** Per preprocessed file occurrence, "which items declare
   `name` in scope path `P`", friend declarations, using-directives, specializations and
   definitions are indexed without semantics, so that invalidation of lookups is decided by hash
   lookups, not by re-parsing.

### B1. Vocabulary and the engine contract

#### B1.1 Regions, trees, prefixes

Preprocessing a file occurrence yields a **region**: its own token stream (with `#include`
directives replaced by references to child regions) plus the macro delta it produced. A region and
its children form a **tree**; the tree id is the hash of all token streams in it. A TU is the tree
rooted at the main file.

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

Per item list (keyed by the content-addressed handle of the list, which is what a prefix node
carries; non-contextual, pure, cheap):

- `region_items(R)`: the item tree, in order.
- `region_decls_named(R, P, name)`: ordered items declaring `name` into scope path `P`
  (including unscoped enumerators into the enclosing scope, injected-class-names, using-declarations
  under the name they introduce, and `using namespace` excluded). Friend declarations are indexed
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
pins the same content-addressed handles (§B1.4); a selection cell's trace names question cells,
never these indexes directly.

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
item-relative positions, never a TU, a slot or a prefix, or two prefixes with identical traces
would hold different results. And a computation asks questions only at its own prefix; an
entity-keyed query asked at prefix `Q` (`class_def(C, Q)`, `instantiate_fn(spec, END)`) locates
the defining item with a question (`definition_item(C, Q)`) and fetches that item's analysis at
the item's own prefix, which is a sub-prefix of `Q`, as an ordinary dependency.

Notation: in the rest of this document `ctx` in a query signature is the prefix argument, and
`at: I` abbreviates the prefix of item `I`.

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
arguments)`; for internal-linkage and local entities, prefixed with the TU or enclosing entity.
Entity ids never change when a redeclaration is added. They key everything program-wide:
`class_def`, `layout`, `function_ir`, `instantiate_*`, `satisfies`, ODR.

### B2. Program points and visibility

Lookup sees declarations that precede the point ([basic.lookup.general], [basic.scope.scope]).
The question "which declarations of `name` in scope `S` precede item `I`" is the query

    visible_decls(S, name, at: prefix)  →  ordered list of (declaration id, flags)

a question cell computed by walking the prefix from the root: at each level,
`list_decls_named(item-list handle, S, name)` for the level's items before the include item, with
each earlier include item contributing `tree_decls_named(tree id, S, name)`; at the leaf, the
item's own region's items before it. All of these are the shared syntactic indexes of §B1.3, so a
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

What the AST must provide: items as the unit of parsing; source positions stored as
`(item, offset)` and only turned into file/line by a late query `item_location(item, ctx)` for
diagnostics and debug info; declaration ids and entity ids as the only cross-item references.

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
nearest namespace enclosing both the directive and `U` ([namespace.udir]/2), `visible_decls(U,
name, at I)`. The set of directives is `directive_closure(at I)`: the using-directives in every
scope of the chain before `I` (block-scope ones are inside the body being analysed and cost no
query; namespace ones are `using_directives(Sk, at I)` per level), closed transitively through
`using_directives(U, at I)` for each nominated `U` ([namespace.udir]/3). The closure is one query
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
`class_decls_named_before(C, name, member item)`. The rule that a name must mean the same thing
when re-evaluated in the completed class ([basic.scope.class]) is IFNDR; we diagnose it by
re-verifying the member's trace with the "before" filter removed once `class_def(C)` is done
(§B6.6 explains the technique).

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

1. `associated(T)` per argument type: namespaces and classes per [basic.lookup.argdep]/3,
   computed from the canonical type, its template arguments, function parameter and return types,
   and the bases of class types (`class_def(C)` fetched when complete; an incomplete class
   contributes itself and its namespace only, and `complete_before(C, at I)` is recorded). Pure
   per type and memoized program-wide.
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
of instantiation uses the same query with the instantiation context's position (§B6.2).

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
contextual query like any declaration's; its context is the template's own position.

#### B6.2 Instantiation and its context

    instantiate_fn(spec entity, prefix)      prefix = END of the TU (default) or a point
    instantiate_class(spec entity, prefix)   prefix = point of first demand in the TU
    instantiate_var(spec entity, prefix)     prefix = point of first demand in the TU

The index is the specialization's entity id (`(template entity, canonical arguments)`), so a
single trie serves the whole program. What the instantiation observed via ADL, partial
specializations, completeness and constant evaluation is in its trace. Two TUs whose END prefixes
agree on every question walk to the same leaf and compute nothing. A TU whose preamble changes an
answer computes a second leaf; `tu(TU)` reports the fingerprint it selected, and `odr_report`
(§B10) diagnoses that the specialization has different meanings in the two TUs ([temp.point]/8
says that is ill-formed, no diagnostic required; we give one).

Function template definitions use the END context because the end of the TU is a point of
instantiation for every specialization that has one ([temp.point]/7), all points must agree or
the program is IFNDR, and END sees the most declarations, which is what the real-world corpus
assumes of compilers. The exception is a specialization needed for constant evaluation while
parsing ([temp.inst]/8): it is instantiated in the context of the enclosing namespace-scope item
and that cell is the one used for code generation. `tu(TU)` also fetches the same index at the END
prefix and compares fingerprints; a difference means two points of instantiation disagree, and we
diagnose it ([temp.point]/8) instead of silently choosing.

Class templates use the point of first demand because completeness is observable and not covered
by the IFNDR rule (`X<S>` instantiated before `S` is complete has a different, valid meaning), and
a class template specialization has at most one point of instantiation per TU ([temp.point]), so
there is nothing to compare within a TU; across TUs, `odr_report` compares the selected results.

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
and orders with `more_specialized(a, b)` ([temp.spec.partial.order]; pure, shared). The context
position is END: a partial or explicit specialization must be declared before the first use that
would select it, in every TU, or the program is IFNDR ([temp.spec.partial.general],
[temp.expl.spec]), so using the TU-wide set is permitted and maximizes sharing. The diagnostic
is again a comparison of two cells: `tu(TU)` knows the prefix of the first demand of each
specialization (it orders demands by source position anyway, §B6.4) and fetches
`select_specialization(primary, args, first-demand prefix)` beside the END one. A different
winner is reported as "specialization declared after a use that it would affect".

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

Which instantiations a TU emits and in what order is `tu_instantiations(TU)`: the set of
specialization entities demanded transitively, ordered by the source position of first demand
(item, offset), never by completion order (DECISIONS §4.4). Diagnostics from an instantiation are
attached to that position.

#### B6.5 Constraints and `requires`

`satisfies(concept entity, canonical args, ctx)` normalizes ([temp.constr.normal]) and evaluates
atomic constraints with `atomic_satisfied(atomic id, mapping, ctx)`, each a substitution followed by
constant evaluation (§B7); `requires`-expressions ([expr.prim.req]) substitute each requirement and
record the lookups, conversions and completeness checks they perform, with substitution failure
as a `false` *value*. The context is the point of first demand, not END: a requirement like
`requires { sizeof(T); }` legitimately differs before and after `T` is defined, and only *checking
it at both points* is IFNDR ([temp.constr.atomic]/3). Memoization by entity id gives the required
caching; a second entry with a different result for the same key is the IFNDR case and is
diagnosed.

#### B6.6 The re-verification technique

Several rules say "if this gives a different answer at another point, the program is ill-formed,
no diagnostic required" ([temp.point]/8, [temp.constr.atomic]/3, [temp.spec.partial.general],
[temp.expl.spec], [basic.scope.class]). Each is implemented by one mechanism: fetch the same
index at the other point's prefix and compare the selected fingerprints. When the program is
well-formed the second fetch is a trie walk, one hash lookup per trace element and no
computation; only when an answer differs is a second result computed, which is exactly the case
being diagnosed. The explanation is the first question on the selected path whose answer differs
at the other prefix ("`f(S, long)` is visible at the end of the TU but not at the point of
instantiation"), computed from live question cells. Divergence between TUs is the same comparison
performed by `odr_report` over the fingerprints each `tu(TU)` selected.

### B7. Constant evaluation and reflection

Constant evaluation runs ycxx's IR in the interpreter (DECISIONS §4.6) inside whichever query
needs the value (an initializer, a template argument, `static_assert`, an array bound,
`if constexpr`, a satisfaction check). It is not itself a query per expression; the memoized
results are `constant_value(variable entity, ctx)` for variables with constant initializers and
`function_ir(entity, ctx)` for callees. What an evaluation records:

- `function_ir(F, ctx)` for every call, and `definition_before(F, at I)`: a constexpr function
  that is not defined at the point of evaluation makes the expression non-constant
  ([expr.const]); the negative answer changes when a definition appears before `I` and stays
  equal when one appears after.
- `constant_value(V, ctx)` for every `constexpr`/`constinit`/const-integral variable read, and
  `definition_before(V, at I)`.
- `layout(C, ctx)` for `sizeof`, member offsets and object representation, `vtable(C)` for virtual
  calls, `class_def(C, ctx)` for member access, `complete_before(C, at I)`.
- For template specializations needed by the evaluation, `instantiate_fn(spec, ctx)` with this
  point's context (§B6.2).

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
`injected_definitions(TU, at I)`, an aggregation of `item_effects` over items before `I`, kept per
region so that a region with no reflection produces an empty value that cuts off. The draft
restricts injection to plainly constant-evaluated contexts, which is what makes `item_effects` a
function of the item rather than of the order in which speculative evaluations happened to run.

Determinism: the interpreter has no clock, no addresses that escape, and deterministic iteration
orders; `__COUNTER__`-like state does not exist at this level.

### B8. Class completeness and layout

- `definition_item(E, ctx)`: which item (or injected declaration) before this prefix defines `E`;
  `complete_before(C, at I)` is `definition_item` compared with `I`, a boolean whose change
  invalidates exactly the observers of (in)completeness: `sizeof` in a `requires`, derived-to-base
  conversions, member access, `C::m`, base specifiers, ADL association by bases, `is_complete`
  tricks.
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
`__TIME__` are an input `build_timestamp` set by the client per build request (default: the
request time); a region that uses them records it and is legitimately re-preprocessed every
build, with a warning. Incremental equals cold because the cold run is given the same timestamp.

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
(a lex and split of the file, milliseconds); item ids are unchanged except that `body_tokens(f)`
changes. Every item of the region has its trace re-asked: each `visible_decls` is a new region
key answered in O(1); all verify. `analyze_body(f)` re-runs, `function_ir(f)` re-runs; if `f` is
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
first-use re-verification reports uses that preceded the new specialization and would have selected
it.

**(iv) Adding a hidden friend to `S`.** `class_def(S)` changes (new item in the class body).
`friend_decls(S, name)` changes for that name only; ADL lookups with `S` associated that asked that
name re-run; `layout(S)` re-runs and cuts off (friends do not affect layout); `class_decls_named`
for other names cut off.

**(v) Adding `#define FOO` to a header `h.h`.** `pp(h.h)` recomputes; its delta now defines `FOO`.
Every later region in each TU that tested `FOO` (`#ifdef FOO`, or merely spelled the identifier
`FOO`) fails its `macro(FOO)` question and re-preprocesses; regions that never mention `FOO`
verify. Re-preprocessed regions with unchanged tokens have unchanged region hashes and nothing
below them moves.

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
- `__COUNTER__` and `build_timestamp` users recompute every time they legitimately must.

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
- **TU-level reanalysis when a header changes.** The status quo this project exists to replace.

### B14. Points settled with Part A

- Contexts are prefixes; traces are complete dependency lists stored as a trie per index;
  divergence is `odr_report` over the fingerprints each `tu(TU)` selected (§A1.9).
- Question cells are recomputed in O(1) from the syntactic indexes and are not persisted; trie
  leaves are persisted as `(index, path, result)` records (§A1.9, §A7).
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
- fork-join where a joiner runs only its own join's tasks;
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
- **Cell storage.** Entries are heap objects; Part A uses dense cell ids in arenas (§A3.3).
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
  pointers: a shared green tree is navigated through a red view that is never stored (§B1.5).
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
- **Contextual results.** The context of a contextual query is a `Prefix`, never a TU or a slot.
  The results of contextual queries mention nothing context-specific (§A1.9).
- **Instantiation requests** carry their request position on the edge (`cx.at(pos)`), never in
  the key (§A1.7).
- **Synthesized declarations** get ids from their origin and an ordinal. These are implicit
  special members, deduction guides, injected-class-names, and declarations created by
  `define_aggregate` or consteval blocks (§B7).
- **Ordering.** Containers iterate deterministically, and no result depends on hash-table order.

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
- **Cancellation.** Solvers poll `check_cancelled()` at their deterministic checkpoints and
  return the heuristic result when cancelled.
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

Follow-up implementation issues are opened when this design is accepted, one per row:

| Issue | Area | Content |
| --- | --- | --- |
| Engine core | `area:query-engine` | Cells, the sharded table, fingerprints, red/green with groups, durability, diagnostics as values with `diags_of`, the query API of §A10 |
| Scheduler | `area:query-engine` | Chase–Lev pool with priorities and compensation, wait-for-graph cycle detection, cooperative cancellation |
| Contextual queries | `area:query-engine` | Prefixes, selection cells, per-index trace tries, `odr_report` |
| Memory and persistence | `area:query-engine` | Interners, three-step eviction, the on-disk cache and loading |
| Incremental == cold harness | `area:tests`, `area:query-engine` | Edit generators, table comparison, `--verify-memo`, chaos scheduler, cancellation injection |
| Item splitter | `area:parse` | The token-level pre-pass that cuts items, and item ids |
