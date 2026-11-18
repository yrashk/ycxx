# ycxx status

What is true of the code today. Work in progress and its owners are tracked in
[GitHub issues](https://github.com/yrashk/ycxx/issues).

## Bootstrapping (2026-11-17)

- Charter written: [README.md](README.md), [DECISIONS.md](DECISIONS.md) (scope, provenance,
  bootstrap, architecture direction, process, coordination), [AGENTS.md](AGENTS.md) (agent guide).
- libycxx added as a submodule at `deps/libycxx`.
- Build and CI skeleton (#4), described in [docs/BUILDING.md](docs/BUILDING.md):
  - CMake builds `build/bin/ycxx` with GCC 16.2 against libycxx's `import std;`.
  - The preprocessor policy is enforced by `tools/check-policy`.
  - CI runs PR, `main` and nightly tiers on Linux x86_64 and macOS arm64.
  - `ycxx --version` works. The fronts (`daemon`, `cxx`, `lsp`, `ctl`) are stubs that exit 2.
- The bootstrap smoke test (`tests/bootstrap`) passes for modules, reflection, contracts,
  coroutines and the other C++26 features ycxx relies on. `std::execution` is an expected
  failure (below).
- Query engine design accepted: [`docs/design/query-engine.md`](docs/design/query-engine.md)
  (#1), summarized in DECISIONS §4. A throwaway prototype is in `spikes/query-engine/`.
- Test driver: `tools/test` ([docs/TESTING.md](docs/TESTING.md), #5). Test kinds accept/reject
  (error locations), `-E` against an oracle, run, differential against Clang 23 and GCC 16, and
  incremental (format and hook; the ycxx side waits for #19). Fail-fast state, `--fast`, tiers,
  failed-first, duration sharding, affected-test hook. A smoke suite of each kind passes with
  Clang and GCC as the compiler under test; with ycxx every test is UNSUPPORTED
  (`tests/features/ycxx.txt` lists nothing yet).
- Optimization design accepted: [`docs/design/optimization.md`](docs/design/optimization.md)
  (#3), summarized in DECISIONS §4.8. Throwaway spikes are in `spikes/optimization/`: e-graph
  extraction, and solver builds against libycxx.
- AST design accepted: [`docs/design/ast.md`](docs/design/ast.md) (#2), summarized in DECISIONS
  §4.9. A throwaway spike is in `spikes/ast/`: a reflection-derived node schema, hash-consing,
  the red view and type canonicalization.

## Query engine core (#14, 2026-11-18)

- `ycxx.hash` (`src/hash/`): MurmurHash3_x64_128 with the fixed seed `hash_seed`, streaming and
  `constexpr`, checked against an independent implementation's vectors. `fingerprint(v)` derives
  a structural fingerprint by reflection; interned handles supply `hash_into`.
- `ycxx.engine` (`src/engine/`), single-threaded and synchronous:
  - inputs with durability, memoized async kinds (`query_task` coroutines), `cheap` and
    `transparent` kinds, `demanded` kinds with demand lists;
  - red/green validation over ordered groups with no short-circuit inside a group, early cutoff
    by fingerprint, the durability shortcut, one revision per batch of input changes;
  - the §A1.6 state word, the §A3.6 layout (cell ids, flat dependency pool with group bits),
    deterministic recording with deduplication (§A3.7);
  - diagnostics as values and `DiagsOf`, pruned by `has_diags`;
  - cycles reported sorted by kind name and key fingerprint (`query_cycle`), with cells reverted;
  - concepts, annotations, reflection-derived serialization and the schema hash (§A10).
- Measured (Release, Apple M-series): a fetch hit inside a query about 40 ns, a root `get` hit
  about 15 ns, fingerprinting a 16- or 32-byte key about 2 ns (`engine_bench`).
- Tests (`tests/unit/`): red/green, groups, cutoff, durability, recording, diagnostics, demands,
  cycles, failures, serialization, and incremental == cold over 1,000 random edits, which fails
  when a dependency is dropped.
- Not yet: the scheduler, `cell_sender` and cancellation (#15; every await completes inline
  today), contextual queries (#16), eviction and persistence (#17; the dependency and diagnostic
  pools only grow).

## AST core (#32)

`src/ast/`, from the [AST design](docs/design/ast.md) §2, §3, §9 and §10. Modules `ycxx.ast.*`,
one import `ycxx.ast`; unit tests in `tests/unit/ast` (run by ctest, see docs/TESTING.md).

- **Handles** (`handles.cppm`): the strong enums of §2 and the closed list `handles`. Each
  handle enum names its interner in an annotation (`[[=interner::names]]`), which the §12 probe
  confirmed GCC 16.2 supports. `SeqId<T>`, `DeclId`, `DeclOrd`, `Loc`, `Child`, `ChildList`.
- **Schema** (`schema.cppm`, `nodes.cppm`, `records.cppm`): node kinds are plain aggregates in
  one closed list, `Nodes`. Reflection checks the §2 invariants on every kind and record (no
  pointer, no `Loc`, only listed handles, edges only in nodes) and the 32-byte payload budget
  (§9.6). It also reads the sequencing annotations of §3.7.4, `[[=sequenced(r)]]`,
  `[[=indeterminately_sequenced(r)]]` and `[[=unsequenced(r)]]`, with ranks ordering operand
  groups. `operands<T>` and `relation<T>(a, b)` expose them.
  - The first list of kinds covers expressions with every sequencing shape, statements and
    attributes. The parser adds the rest.
  - `ItemSema`, `DeclSema`, `ClassDef` and `BodySema` follow §3.6. `ParamSema`, `BaseSpec`,
    `MemberRec`, `LocalRec` and the enums are first cuts for sema to fill in.
- **Interners** (`interner.cppm`):
  - Per-kind untyped pools indexed by kind number, never a tuple, plus one edge pool.
  - Identity is the 128-bit Merkle fingerprint. Debug builds compare structurally on a hit.
  - Fingerprint, equality and the handle walk `for_each_ref` are derived by reflection, and
    dispatch goes through a generated table.
  - Interners: `names` and `seqs` (one pool per element type) and `ast`.
  - `tokens`, `items`, `entities`, `types` and `values` are placeholders that hold only
    fingerprints, until #18 and #33.
  - Single-threaded: sharing between workers is the engine's (#14).
- **Fingerprint seam** (`fingerprint.cppm`): a stand-in hash. `ycxx.hash` (#14) has landed;
  switching to it changes only that file plus `hash_into` for the AST handles, a follow-up on #32.
  Nothing persists these fingerprints yet.
- **Frame builder** (`builder.cppm`): drafts in the frame's arena (a `std::pmr` resource).
  `commit(value)` interns what the value reaches, bottom-up, so abandoned drafts leave nothing
  (§9.3). Lists of records that hold nodes are drafts; other lists are interned eagerly.
- **Red view** (`red.cppm`): a stack cursor with item-relative offsets and the parent chain, and
  `find(offset)` for descent.
- **Compaction** (`compact.cppm`): holders mark live values, the survivors are renumbered with
  their fingerprints kept, and holders rewrite their values with the returned `Remap`. Covers
  `ast` and `seqs`.

## Bootstrap workarounds

Places where ycxx's sources avoid or work around a C++26 feature because GCC 16 lacks it or
miscompiles it (DECISIONS §3.2). Each is removed once ycxx compiles itself.

- **`std::execution` through `import std;` does not compile** (GCC 16.2 ICE in `tsubst_expr`).
  - Cause: a GCC modules bug. An exported concept whose nested requirement uses a
    requires-parameter inside a template argument crashes the compiler. libycxx's
    `std::unstoppable_token` has that shape.
  - Affected: anything that checks `std::unstoppable_token`, including `parallel_scheduler`,
    `when_all`, `counting_scope` and `task`. This blocks the query engine (DECISIONS §4.3).
  - Fix: belongs in libycxx (yrashk/libycxx#4, which has a 10-line repro and a tested
    workaround). Then bump the submodule and drop `_xfail_execution` in
    `tests/bootstrap/CMakeLists.txt`.
- **Defaulted comparisons are members, not hidden friends** (`src/ast`, `ycxx.hash`'s `Fp128`).
  - Cause: GCC 16.2 crashes (segfault) in an importer that compares two values of an exported
    struct whose defaulted `operator==` is a friend, with or without `-freflection` (found by
    #14, #23 and #32).
  - Fix: belongs in GCC. Restore the friend form when ycxx compiles itself.
- **Checked `constexpr` constructors of handles are not `constexpr`** (`make_node`,
  `make_qual_type`, `make_decl_ord` in `src/ast/handles.cppm`).
  - Cause: GCC 16.2 does not emit its TU-local contract-violation helper
    (`__tu_has_violation`) in an importer that inlines a non-template inline function with a
    precondition. The importer fails to link unless it has a contract of its own. Templates and
    in-class member functions are not affected.
  - Fix: belongs in GCC.
- **A contract predicate inside a lambda that names a member of the enclosing class goes through
  a local `bool`** (`src/ast` interner, builder, compaction).
  - Cause: in such a predicate GCC 16.2 takes `this` to be the closure object, so the member
    read is garbage. The lambda's own captures read correctly, and so does the same expression
    in the lambda body. It happens in a plain TU, with generic or plain lambdas, `contract_assert`
    or the lambda's `pre`, at -O0 and -O2:

        struct S { int x = 42;
          void f() { [this] { contract_assert(x == 42); }(); } };  // reported violated
        int main() { S{}.f(); }

  - Fix: belongs in GCC. Restore the plain form when ycxx compiles itself.
- **`pre` on a function template is dropped when it is instantiated through an import** (GCC
  16.2; non-template functions keep theirs). `Db::set` asserts its precondition with
  `contract_assert` in the body instead.
- **The std module's BMI must be built with `-freflection -fcontracts`.**
  - `CMakeLists.txt` adds the flags to libycxx's `ycxx_modules` target.
  - Without them, `std::meta` is empty. An importer whose `-freflection` does not match the
    BMI's segfaults GCC 16.2.

## Milestones

| | Milestone | Done when |
| --- | --- | --- |
| M0 | Foundations | Query-engine, AST and determinism designs accepted; build, CI and test driver run; draft examples extracted |
| M1 | Preprocessor | `-E` matches Clang's on the oracle corpus; incremental macro-dependency caching |
| M2 | Parser | The full grammar parses libycxx, ycxx and the real-world corpus |
| M3 | Semantic analysis | Accept/reject agrees with the draft examples and the oracles |
| M4 | IR and interpreter | The own suite and libycxx tests run in the interpreter |
| M5 | AArch64 Mach-O | Native objects and our own linker; mixed-ABI tests with Clang pass |
| M6 | x86-64 ELF | Same on Linux |
| M7 | libycxx under ycxx | libycxx's suite passes, compiled by ycxx |
| M8 | Self-hosting | Stage 2 equals stage 3; the GCC-built bootstrap anchor is released (DECISIONS §3.5) |
