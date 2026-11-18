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

## Lexer and directive-free `pp` (#40, 2026-11-18)

- `ycxx.lex` (`src/lex/`): translation phases 1–3, 5 and 6 as functions.
  - Phases 1–2: UTF-8 checked, CR LF and CR become LF, a leading BOM is deleted, splices removed,
    with maps from logical offsets back to phase-1 text (raw strings revert splices) and to
    physical byte offsets.
  - Phase 3: every [lex] preprocessing token. UCNs (`\u`, `\U`, `\u{}`, `\N{}` with names and
    aliases), XID and ID_Compat_Math identifiers with the NFC check, maximal munch with the
    `<::`, `[::` and `[:>` exceptions, raw strings, digraphs and alternative tokens, header names
    on request.
  - Phases 5–6: keywords, integer and floating literals (UDL suffixes per [lex.ext]),
    character literals, and string-literal concatenation with the common encoding. Values:
    integers to 64 bits, `double` and `float` correctly rounded, code units for characters and
    strings. Ordinary literals are UTF-8 and wide ones UTF-32.
  - `Tk` and the diagnostic table are derived by reflection from annotated enumerators.
  - Unicode 17.0 tables from `tools/unicode/gen-tables` (`unicode_tables.cppm`,
    `unicode_names.txt`). Name aliases come from Unicode 13 data (Perl's), checked against 17.0.
- `ycxx.pp` (`src/pp/`): `pp(FileId)` over the input `FileText`, valued as a `Region` of tokens,
  `TokLoc`s, names and literals. Every directive (`#…`, and the `module`/`import` lines of
  [cpp.pre]) is diagnosed as unsupported and skipped. Diagnostics are at
  `Loc{file_level, byte offset}`.
- Measured (Release, Apple M-series, `lex_bench`): about 25 MB/s, 110 ns per token. Not tuned.
- Not yet: phase 4 (M1), `long double` and the extended floating types' values (left to the
  target), the type of an integer literal (sema, #33).

## Item splitter (#18, 2026-11-18)

- `ycxx.items` (`src/items/`): `split(region, file)` and the query `RegionItems(FileId)` cut a
  region into items before parsing (query-engine.md §B1.2), preorder with nested lists for
  namespaces, linkage and export blocks and class bodies.
  - Bodies: compound statements, ctor-initializers, function-try-blocks, `= default`,
    `= delete(…)`; consteval blocks. Template-heads attach to what follows.
  - Skeletons (ast.md §3.1): kind, declared names in token order with ordinal, offset and flags
    (type, template, namespace, unknown, definition, friend, enumerator), the first declarator's
    spelled qualifier, template depth, body range.
  - Ids: 128-bit fingerprints over the file identity, scope path, declaration-part tokens (kind
    and content, not whitespace or alternative spelling) and the ordinal among siblings. A
    namespace's id excludes its members; a class's excludes its members' bodies. Bodies are
    content-addressed.
- Token heuristics, not a parse: a few forms are known to be guessed wrong (`S (x);`,
  `decltype(b){1}` as a mem-initializer, template arguments with commas in an initializer).
- Not yet: `ItemId`/`BodyId` handles from the `items` and `tokens` interners (fingerprints only
  until #38's placeholders are replaced), the occurrence salt (needs prefixes, #16), flattening
  child regions (needs includes, M1), the syntactic indexes of §B1.3.

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
- **A defaulted friend comparison in an exported class ICEs GCC 16.2** in the importer
  (segfault), with or without `-freflection`. `ycxx.hash`'s `Fp128` uses member operators.
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
