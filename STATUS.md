# ycxx status

What is true of the code today. Work in progress and its owners are tracked in
[GitHub issues](https://github.com/yrashk/ycxx/issues).

## Embedding

- CMake builds independently linkable static or shared libraries for the query engine,
  preprocessor, front end, IR, object formats, linker, compiler and project build.
- The bootstrap build can use libycxx or GCC's system libstdc++. The system build uses
  libycxx's portable execution provider for senders, scopes and the scheduler.
- The CMake SDK exports typed C++ module interfaces. The engine and compiler samples exercise
  build-tree and installed-package consumption. See [the embedding instructions](docs/BUILDING.md#linking-ycxx-into-a-c-application).

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
  coroutines, `std::execution` and the other C++26 features ycxx relies on.
- Query engine design accepted: [`docs/design/query-engine.md`](docs/design/query-engine.md)
  (#1), summarized in DECISIONS §4. A throwaway prototype is in `spikes/query-engine/`.
- Test driver: `tools/test` ([docs/TESTING.md](docs/TESTING.md), #5). Test kinds accept/reject
  (error locations), `-E` against an oracle, run, differential against Clang 23 and GCC 16, and
  incremental (format and hook). Fail-fast state, `--fast`, tiers, failed-first, duration
  sharding, affected-test hook. A smoke suite of each kind passes with Clang and GCC as the
  compiler under test; ycxx runs what `tests/features/ycxx.txt` lists.
- Incremental == cold (#19): ycxx's `.incr` side is a private resident daemon per test, with each
  request repeated (no engine cell may execute) and each edit costing no more than a fresh
  daemon; `tools/incr-fuzz` (random token edits, `-fsyntax-only`, delta-minimized `.incr`
  regressions). Not yet: `ctl verify-table` (#146, after #137), git-history replay, regrouping
  and nightly fuzzing (#147), `--verify-memo` (#148).
- Optimization design accepted: [`docs/design/optimization.md`](docs/design/optimization.md)
  (#3), summarized in DECISIONS §4.8. Throwaway spikes are in `spikes/optimization/`: e-graph
  extraction, and solver builds against libycxx.
- `ycxx.cm` in `src/cm/` models bounded integers, Boolean 0/1 views, linear rows and
  minimization. It emits stable FlatZinc with explicit failure for values beyond the
  offline solver's integer range. Optional CTest runs exported models through Chuffed.
  The standalone solver build pins HiGHS, CaDiCaL, and Chuffed by revision and archive
  hash. See [optimizer experiments](docs/BUILDING.md#optimizer-experiments) and [#66](https://github.com/yrashk/ycxx/issues/66).
- Experimental x86-64 Linux optimization rewrites unsigned division and remainder by
  powers of two. The solver mode also selects compatible quotient/remainder pairs using
  an exact prefix model with emitted byte costs. The query engine traces modeling, solving,
  validation and fallback. [Unsigned division experiment](docs/design/optimization.md#93-unsigned-division-on-x86-64).
- `ycxx.bench` in `packages/ycxx-bench/` provides calibrated, warmed sampling and balanced
  comparisons, with typed callable properties and metadata. It reports clock and
  host context, retained samples, and practical comparison verdicts. The optimizer
  uses it for FlatZinc export timing. [Benchmark runner](packages/ycxx-bench/GUIDE.md).
- AST design accepted: [`docs/design/ast.md`](docs/design/ast.md) (#2), summarized in DECISIONS
  §4.9. A throwaway spike is in `spikes/ast/`: a reflection-derived node schema, hash-consing,
  the red view and type canonicalization.
- L1 IR (#23) in `src/ir/` (module `ycxx.ir`), as [`optimization.md`](docs/design/optimization.md)
  §2.1–§2.4 define it:
  - The op table is the enumerators of `ir::Op`, each annotated with an `OpSpec` (node class,
    operand and result kinds, immediates, successors, accepted flags, traits); `op_table` is built
    from them by reflection. `eval_table<Impl, F>()` is the L1 evaluator hook: it gathers an
    evaluator's static members named after the ops, and with `complete` a missing op does not
    compile.
  - `Builder::finish` canonicalizes a body (blocks in reverse post-order, nodes in operand order,
    readers of a state before the node that moves it on, dense ids) and `verify` checks it against
    the table, dominance and the single state chain. `fingerprint` hashes the canonical body,
    with handles hashed through their own fingerprints. `FunctionDebug` and `OriginMap`
    (compose, remap) hold positions apart from the body.
  - A text printer and parser round-trip every op (`tests/unit/ir`).
  - `src/ir/fingerprint.cppm` adapts `ycxx.hash`, and `src/ir/handles.cppm` re-exports
    `ycxx.ast.handles`. `LocalId` is still a stand-in. Bodies hash handles with
    `stand_in_handles` until the interners supply Merkle fingerprints, and types print as `T<n>`.

- Draft examples extracted (#6): [`tests/draft`](tests/draft/README.md), generated by
  `tools/draft-examples` from pinned cplusplus/draft and CWG-list commits (`tests/draft/PINS`).
  - 894 examples in [lex]–[except], [cpp] and [temp]: 371 accept tests, 323 reject tests and
    200 skips with recorded reasons (122 fragments, 29 prose-only, 23 multi-unit, 11 ill-formed
    NDR, 15 other).
  - Clang 23 and GCC 16 disagree with the draft on 204 tests, both on 104. The tests carry an
    `XFAIL` per oracle, and `tests/draft/DISAGREEMENTS.md` lists them with the CWG issues on each
    subclause.
  - The CWG issues list is imported as `tests/draft/cwg-issues.tsv`: 3,240 issues with subclause,
    status and resolution.
- Compiler compatibility modes select behaviors from the pinned draft and ABI (default), GCC
  16.2, or upstream Clang 23.1 with `-fcompat=spec|gcc|clang`. The flag is part of the
  translation-unit configuration and is read from editor compilation commands. The
  [compatibility table](docs/compatibility.toml) records prior choices, mode behavior and probes. Constrained function
  templates use the current `Q` and `Tk` forms for supported requires-clauses and type constraints.
  Mode-specific symbols also cover nested and lambda templates, including written folds,
  member and allocation expressions, casts, and target floating-point literals. The table records
  the fallback for builtin expressions whose symbols the two compilers cannot emit.
  GCC mode selects the last nonzero class-head alignment and carries a preceding alignment
  into a definition that supplies none. Clang mode checks alignment consistency on opaque
  class declarations. The compatibility table records Clang's underalignment case separately.
  Spec mode distinguishes C and C++ function types and encodes the C type with `FY` in a
  mangled signature. GCC and Clang modes use one type for both. C function names retain their
  C linkage and unmangled symbols in every mode.
  Compiler modes also select the documented wide array bound conversions, constexpr byte
  comparisons, empty union initializers, abstract result checks, and over-width bit-field layout
  and diagnostics.
  [#236](https://github.com/yrashk/ycxx/issues/236)
- The C++26 feature map (#282): `tools/feature-map/feature-map` over `tools/feature-map/features.toml`.
  180 features at the grain a user names, citing every leaf clause of the pinned draft's core
  chapters, each with its standard, its uses in a 1.9-million-line corpus (libycxx's 19 real-world
  projects at their pinned commits, libycxx and ycxx), a probe program run with ycxx and checked
  with GCC or Clang, its status and its issue. `feature-map check` (in `tools/check-policy`)
  fails on an uncited clause or a non-done row without an issue. CI's macOS job runs the probes
  and `check --probes`, uploads the rendered table as the `feature-map` artifact, and on `main`
  rewrites #282's body; nothing generated is committed (#366).
- `ycxx.hash` (`src/hash/`): MurmurHash3_x64_128 with the fixed seed `hash_seed`, streaming and
  `constexpr`, checked against an independent implementation's vectors. `fingerprint(v)` derives
  a structural fingerprint by reflection; interned handles supply `hash_into`.
- `ycxx.engine` (`src/engine/`):
  - inputs with durability, memoized async kinds (`query_task` coroutines), `cheap` and
    `transparent` kinds, `demanded` kinds with demand lists;
  - red/green validation over ordered groups with no short-circuit inside a group, early cutoff
    by fingerprint, the durability shortcut, one revision per batch of input changes;
  - the §A3.6 layout (cell ids into an append-only arena, a sharded index per kind),
    deterministic recording with deduplication (§A3.7);
  - diagnostics as values and `DiagsOf`, pruned by `has_diags`;
  - concepts, annotations, reflection-derived serialization and the schema hash (§A10).
- The scheduler (#15, 2026-11-18), in `src/engine/engine.cppm` and `pool.cppm`:
  - claims are CASes on the §A1.6 state word; waiter lists with the `CLOSED` sentinel; a frame
    that waits suspends and frees its worker; completions queue continuations, never resume them
    on their own stack;
  - `Pool`: Chase–Lev deques per worker and priority, stealing interactive work first, an
    injection queue, parking; `pool_scheduler` (a `std::execution` scheduler) and `pool_backend`
    (`parallel_scheduler_replacement`'s backend);
  - the chaos scheduler: a seeded `Pool` that reverses deques, steals first, migrates tasks and
    delays fetch misses;
  - requests as senders (`Db::request`, `cell_sender`); `Db::get` blocks; a request completes
    only after every frame it started, so finished requests mean quiescence;
  - cancellation by stop tokens: a stopped fetch never resumes its query, the cell reverts, and
    waiters look again and claim it; `cx.stop_point()` for long leaves;
  - cycles found by the wait-for walk (and on the synchronous chain of cheap kinds), reported
    sorted by kind name and key fingerprint, delivered as `set_error`, cells reverted;
  - `Epochs`: requests in revision epochs (`counting_scope`), `apply` at quiescence, interrupted
    requests respawned, the progress quantum's hook (daemon design §4.1–4.2);
  - a `Db` without a pool runs everything on the calling thread (what the pipeline uses).
- For the daemon (#95, daemon design §4.4, §5.1, §6.3, §8.1):
  - durabilities `editor < ambient < low < medium < high`;
  - input providers (`Db::set_provider`): an input read before anyone set it is provided then,
    with the provider's durability, at the current revision and with no new one;
  - `Db::set` returns whether the value changed; `Db::reset_unobserved` changes an input nobody
    has read since it was set, while requests run (an "observed" flag per input cell, set by
    every read and validation, paired with the reset's claim);
  - `Counters` per request (`Db::request(k, p, &counters)`, `Epochs::Ticket::stats`), per worker
    for the Db, and per query kind when `count_per_kind(true)`.
- Where the scheduler differs from query-engine.md (#108):
  - a frame that leaves without publishing reverts its cell and wakes every waiter to look again,
    instead of handing the claim to the first waiter (§A3.1);
  - every waiter of a cell that failed gets its error, not only waiters on the cycle: a retry
    would fail the same way (§A2.3, §A3.1);
  - children are queued rather than started by symmetric transfer, and a stop is noticed at a
    fetch miss, not at every fetch;
  - `query_task` is not a sender; requests are (`cell_sender`).
- Measured (Release, Apple M-series): a fetch hit inside a query about 50 ns (40 ns before the
  scheduler; the difference is the shard lock), a root `get` hit about 17 ns, fingerprinting a
  16- or 32-byte key about 2 ns (`engine_bench`).
- Tests (`tests/unit/`): red/green, groups, cutoff, durability, recording, diagnostics, demands,
  cycles, failures, serialization, and incremental == cold over 1,000 random edits, which fails
  when a dependency is dropped (`unit.engine`, single-threaded). `unit.engine_scheduler`: the
  pool, priorities and backend; outputs, dependency lists and execution counts equal at 1, 2, P
  and 2P threads with the chaos scheduler; incremental == cold at each; cancellation injected;
  hand-over from a stopped owner; respawn; cycles and errors from every entry point. `Db::verify_table`
  is §A9's memo-table comparison against a cold `Db` (#19); both fuzzes use it. `unit.engine_inputs`: providers (once per cell under
  contention, durability, failure), the `editor` and `ambient` shortcuts, `reset_unobserved`
  racing readers over 300 rounds, counts per request and per kind. Clean under
  ASan and UBSan. ThreadSanitizer runs nightly on Linux (`tools/tsan`, `linux-x86_64-tsan`; #105).
  The deque stores `bottom_` with release, not a release fence, since ThreadSanitizer does not
  model fences.
- Not yet: allocation-free waits, per-worker frame free lists and lock-free index reads
  (#104); the cycle report's program-order path note (#106); contextual queries (#16);
  eviction and persistence (#17).
- A session on a pool (#137):
  - `Pool::set_scope` makes each worker enter a scope before it runs work. A
    `pipeline::Session(Pool&)` uses it to bind its universe and interners on the worker's
    thread-locals.
  - pp's per-configuration process sessions have a mutex each, held for a whole `Pp` run (which
    never suspends).
  - sema's records (#156, #115): `Entities` keeps records in an `ast::Published` table (by
    handle, published once, read without a lock). Readers use only the fields that are part of
    the entity's identity, which equal records share; a variable's type, which is not, comes
    from its declaration. Class and enumeration records are each unit's
    (`Program::classes`, `sema::Classes`): entities are program-wide, but a session may see
    different definitions of one (an edit, or several programs in one daemon), and the records
    follow the unit's source. A member's declaration is an item of the unit (`member_decl`).
    Sema reads a record through `class_rec`, only where the class is complete, and an
    enumeration's own list sees its record so far. Each unit's unnamed namespace is its own
    entity. The back end lays out from the unit's `FrontResult::shapes` (fields' types,
    enumerations' underlying types) through `UnitShapes`, so a body edit stops at its cutoff.
  - Tests: `unit.pp` runs 48 units on 8 workers. `unit.pipeline_incremental` builds a program on
    2 and 8 threads and under chaos seeds, equal to an inline build, and eight units sharing a
    header of classes and enumerations on up to 8 threads, before and after an edit; two units
    each diagnose their own ill-formed class; qualified enumerators in their own list and classes
    in two units' unnamed namespaces, in either order. All are mutation-checked.

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
    `unicode_names.txt`): Python's `unicodedata` and the vendored UCD `NameAliases.txt`
    (`tools/unicode/ucd/`). Unicode License v3 in `src/lex/LICENSE.unicode`.
- `ycxx.pp`: phase 4 since #85; see "Preprocessor" below.
- Measured (Release, Apple M-series, `lex_bench`): about 25 MB/s, 110 ns per token. Not tuned.
- Not yet: the extended floating types' values (#351); `long double`'s are binary64's, the
  target's (sema checks the format); the type of an integer literal (sema, #33).

## UTF-16 and UTF-32 string literals (#288, 2026-11-20)

- `u` and `U` string literals are lvalue arrays of `const char16_t` and `const char32_t`,
  respectively ([lex.string]). Their bounds count the encoded code units and the terminating
  null. The same units initialize matching character arrays and are available to constant
  evaluation ([dcl.init.string], [expr.const]).
- ELF and Mach-O put these literal objects in read-only data at the element type's alignment.
- Tests in `tests/own/unicode-strings/` cover surrogate pairs, escape and raw forms,
  concatenation, array initialization, and constant reads and addresses of literal objects.

## Preprocessor (#85, 2026-11-18)

- `ycxx.pp` (`src/pp/`): phase 4 as query-engine.md §B9's contextual query, with a local stand-in
  for the engine parts that do not exist yet (#16, #93).
  - Each file occurrence is a `FileRegion` (`:region`). It holds its own tokens and its child
    regions, its macro delta and `#pragma once` marks, its `TokLoc` and `Expansion` tables
    (ast.md §8.1), its diagnostics, and its *trace*. The trace lists the questions its entry state
    answered: `macro(name)` for every identifier tested that the region had not defined itself,
    `defined(name)`, `once(file)`, `__COUNTER__`'s value, the build time, include resolutions
    and file contents.
  - A `Session` (`:unit`) caches regions by file occurrence and reuses one wherever its trace
    verifies. A header is reused across TUs and within one, and only a header whose answers
    changed is recomputed.
  - Macro answers are location-free. A region refers to a definition it did not make through a
    binding that its includer resolves.
  - A file that is one `#ifndef X` group records its guard. Included again where X is defined, it
    is an empty region whose one question is X's, so it is not read (§B9).
  - `Region` is the TU flattened after phases 5–6, which the item splitter and the parser read.
    Its `files` are paths with presumed names, so its value does not depend on the session.
    `Region::site(tok)` gives a macro token's expansion site.
- Directives:
  - `#include` in both forms and the macro-replaced form, and `#include_next`;
  - search directories from `-I`, `-isystem` and `-iquote`, then libycxx's headers and the
    target's system headers (`:system`, configured by CMake). For x86-64 Linux, ycxx searches its
    resource headers (#133). A native Linux build also searches `/usr/include/x86_64-linux-gnu`
    and `/usr/include` (#252). Darwin searches the configured SDK's `usr/include`, with a request's
    `SDKROOT` as an override. Cross-target requests without a target SDK can use explicit include
    directories (#690). `-nostdinc` omits those automatic include roots, including the
    bundled library and resource headers, while keeping explicit `-I`, `-isystem`,
    and `-iquote` paths, source-relative quoted includes, and macros.
    [#251](https://github.com/yrashk/ycxx/issues/251)
  - `-nostdinc++` omits the implicit libycxx source and generated include directories. The
    compiler keeps its resource directories, target C library or SDK directories, and explicit
    includes. With both flags, `-nostdinc` applies in either order.
    [#997](https://github.com/yrashk/ycxx/issues/997)
  - `-print-file-name=include` prints the primary configured resource include directory, and
    `-dumpmachine` prints the canonical target triple. The driver accepts `--target` for both
    queries and answers without opening a translation unit or starting a daemon. Linux reports
    `resources/linux/include` and Darwin reports `resources/common/include`. [#970](https://github.com/yrashk/ycxx/issues/970)
  - `-ftrivial-auto-var-init=uninitialized` selects the default automatic-variable
    initialization behavior. The option adds no source configuration state, so editor checks
    use the same behavior when they omit it from imported compile commands.
    [#1015](https://github.com/yrashk/ycxx/issues/1015)
  - `#define` and `#undef` with the redefinition check, rescanning and painting
    ([cpp.rescan]);
  - function-like macros (`:unit`'s `invocation`, `substitute`, `paste`): arguments across
    lines, argument counts, omitted variable arguments, `#`, `##` with placemarkers,
    `__VA_ARGS__`, `__VA_OPT__`, and the constraints on replacement lists. Arguments are
    replaced in a barrier context. Their tokens sit in expansions of kind argument, and pasted
    tokens in kind paste, so `Region::site` leads to the invocation;
  - conditional inclusion (`#if`/`#ifdef`/`#ifndef`/`#elif`/`#elifdef`/`#elifndef`/`#else`/
    `#endif`). Lexer diagnostics on skipped lines are dropped. `#if` expressions are evaluated
    in `intmax_t`/`uintmax_t` (`:cond`), with `defined`, `__has_include`,
    `__has_include_next`, `__has_embed`, and the `__has_*` probes answering 0;
  - `#line` (presumed lines and names), `#error`, `#warning`, `_Pragma`, `#pragma once`
    (other pragmas pass through to `-E`), and the null directive. Compiler diagnostics report the
    text of `#error` and `#warning` in the main file and included headers (#172);
  - module and import lines go on to phase 7, their keywords flagged `tok_flag::directive`.
    Header-unit imports are diagnosed as unsupported.
  - `#embed` and `__has_embed` with `limit`, `prefix`, `suffix` and `if_empty` (#125).
    The offset spelling follows the mode, using `offset` in spec, `gnu::offset` in GCC, and
    `clang::offset` in Clang. GCC mode also accepts double underscores around either identifier.
    Resources use the include search paths and are tracked by resolution and content. A nonempty
    embed is one byte-carrying phase-4 token with an expansion that names the resource. It yields
    integer and comma tokens in the parser's input. Unsupported scoped parameters are diagnosed
    on the directive and cause a probe to return `__STDC_EMBED_NOT_FOUND__` (#473).
- The driver and editor honor `-fno-exceptions`. Semantic analysis rejects active `throw`,
  rethrow and block `try`, with checks in template bodies deferred to instantiation. Native
  lowering keeps ordinary destruction and eligible cleanup-free frame information.
  [#980](https://github.com/yrashk/ycxx/issues/980) · [Design](docs/design/freestanding-no-exceptions.md)
- Predefined macros (`:predefined`, DECISIONS §1.5):
  - `__ycxx__` and its version, `__GNUC__` 4.2.1 (#229), and the draft's macros;
  - `__STDC_HOSTED__` is `1` by default and `0` with `-ffreestanding`. The compiler and
    editor honor the flag, and the mode is part of the source configuration.
    [#973](https://github.com/yrashk/ycxx/issues/973) · [Design](docs/design/freestanding-mode.md)
  - a feature-test table, including `__cpp_pp_embed`;
  - the target's macros, generated from `ast::Target` and checked against Clang on AArch64
    Darwin and x86-64 Linux (`tests/own/pp/predefined-target*.pp.cpp`, #132);
  - x86-64 Linux `-fPIC` defines `__PIC__` and `__pic__` as `2`, and `-fpic` as `1`; the last of
    `-fpic`, `-fPIC` and `-fno-pic` wins. arm64 Darwin always defines both as `2` and accepts the
    same options, as Clang does. The compiler and editor include the mode in the source
    configuration.
    [#983](https://github.com/yrashk/ycxx/issues/983) · [#1089](https://github.com/yrashk/ycxx/issues/1089) · [Design](docs/design/freestanding-pic.md)
  - `__LINE__`, `__FILE__`, `__COUNTER__`, `__DATE__` and `__TIME__`.
- `#include <cstdio>` preprocesses until libycxx's `#error` for unknown compilers
  (yrashk/libycxx#17).
- `ycxx cxx -E` prints what Clang's `-E` would, by Clang's line and spacing rules. `preprocess`
  is in `tests/features/ycxx.txt`. Diagnostics in included files name their file. The default
  target follows the configured compiler host. An explicit `--target` also selects the
  preprocessing target.
- Tests:
  - `tests/own/pp` (by spec-only authors): 103 tests (`-E`, reject and accept), the draft's
    [cpp.scope], [cpp.subst], [cpp.stringize] and [cpp.concat] examples among them;
  - `tests/own/slice/a3-local-header.run.cpp`;
  - `unit.pp`: resolution, traces, reuse across TUs, and incremental equals cold over edit
    sequences.
- Files as engine inputs (#93, 2026-11-18):
  - `Pp` fetches every file it reads as `pp::FileContent{PathId}`, keyed by an interned absolute
    path whose fingerprint is its spelling, and relative paths resolve against the unit's working
    directory. The unit's name, working directory and configuration are `pp::SourceOf`. The
    process keeps one pp session (region cache) per configuration.
  - A `Db` provides `FileContent` on first read (`pp::read_disk`; `pipeline::Session` installs it).
    The daemon provides it from its file table instead.
  - `unit.pipeline_incremental` checks header edits and two directories.
- Not yet:
  - items cut per region; a header's items carry the main file's identity (#126);
  - sema diagnostics in headers, and presumed `#line` locations outside phase 4 (#34).

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

## Mach-O objects and the linker (#42, 2026-11-18)

- `ycxx.obj` (`src/obj/`): the in-memory object. Sections are named the Mach-O way and hold bytes
  (or a zero-fill size) and relocations. Symbols are local, global or hidden, possibly weak.
  Relocations are `branch26`, `page21`, `pageoff12`, `got_page21`, `got_pageoff12`, `abs64`,
  `abs32` (differences only) and `got_rel32`, with explicit addends. Lists keep build order (§A5.3).
- `ycxx.obj.macho`: the arm64 MH_OBJECT writer and reader. The reader takes Clang and GCC objects
  as black-box inputs. A common symbol becomes a weak definition in its own `__DATA,__common`
  zero-fill section. The reader rejects absolute and indirect symbols, alignments above 2^15,
  scattered and TLV relocations, and section-relative relocations other than UNSIGNED.
  A SUBTRACTOR must be followed immediately by a non-PC-relative UNSIGNED at the same address
  and width. Both records describe a 32-bit or 64-bit field. [#702](https://github.com/yrashk/ycxx/issues/702)
  PC-relative `POINTER_TO_GOT` words include the negative offset of their field within the section.
  The reader and writer translate between this word and the model's addend for `GOT(S) + A - P`.
  [#782](https://github.com/yrashk/ycxx/issues/782)
- `ycxx.link` (`src/link/`): `link(objects, options) -> expected<bytes, errors>`.
  - Diagnoses duplicate and undefined symbols and a missing entry point. A strong definition beats
    a weak one; common symbols merge to the largest. `__mh_execute_header` and `___dso_handle` are
    defined by the linker. A weak reference that no dylib exports is imported weakly (0 at run time).
  - Imports come from the dylibs in `options` (libSystem by default), checked against export
    lists read from `.tbd` files (`tbd_exports`). A dylib without a list accepts any name.
  - Lays out `__PAGEZERO`, `__TEXT`, `__DATA_CONST`, `__DATA` and `__LINKEDIT` on 16 KiB pages,
    with stubs and a GOT. All imports are bound and all pointers rebased through chained fixups
    (`DYLD_CHAINED_PTR_64_OFFSET`). No lazy binding. A segment's chained starts cover only its
    pages with file contents: page-in linking maps every listed page from the file, so a listed
    zero-fill page would hold the bytes that follow it in the file (for `__DATA`, `__LINKEDIT`)
    (#842).
  - Writes the exports trie, symbol tables, `LC_MAIN`, `LC_BUILD_VERSION` (macOS 12.0), an
    `LC_UUID` fingerprinted from the image, and an ad-hoc, linker-signed SHA-256 code signature.
  - Output is byte-identical across runs.
  - Weak definitions coalesce: the first in input order is kept, and a strong one beats them.
    The functions and variables the ELF path makes weak (#422, #432) are weak Mach-O definitions
    too, with `N_PEXT` when hidden (#602). Each definition carries its own weak binding, so a
    strong definition sharing its spelling through an `asm` label retains priority (#694).
    Zero-initialized writable weak definitions go in `__DATA,__data`. An object with a weak
    definition sets `MH_SUBSECTIONS_VIA_SYMBOLS`, so the system linker also keeps one. A name
    some object defines with default visibility is exported even when the kept definition is
    hidden, as the system linker does. `unit.darwin_inline` checks the `unit.linux_inline` fixtures
    and mixed content (literals, initializers, guarded statics) with ycxx's and the system's
    linker, dead stripping included, in both object orders, with a host-compiler peer and an
    archive member. Templates, closures and static locals of inline functions remain strong (#124).
  - The linker preserves cleanup-free `__TEXT,__eh_frame` contributions with the supported
    PC-relative CIE and FDE format, including address differences and multiple input objects.
    `unit.link` checks the linked ranges and walks through a non-frame-pointer arm64 bridge
    with the system unwinder on macOS. It also checks the version 1 cleanup-only `zPLR` profile,
    including its relocated personality GOT cell, owned `__gcc_except_tab` pointer, and cleanup
    call-site ranges. The macOS cleanup fixture uses a platform-linked typed handler dylib.
    [#783](https://github.com/yrashk/ycxx/issues/783) tracks this linker slice. Compact unwind and
    other profiles are tracked in [#72](https://github.com/yrashk/ycxx/issues/72).
  - The arm64 compiler emits these DWARF records for proven cleanup-free functions. Their FDEs
    describe the actual saved frame-pointer and return-address locations at the entry, body,
    and return instructions. `unit.darwin_cleanup_free_unwind` checks the emitted object,
    omitted frames, native YCXX linking and system-linked exception propagation on macOS.
    The shared proof includes selected defaults in aggregates with scalar fields and trivial
    destruction and indirect calls with fixed scalar prototypes.
    [#849](https://github.com/yrashk/ycxx/issues/849) · [#851](https://github.com/yrashk/ycxx/issues/851)
    Native throw lowering and landing pads are tracked in [#648](https://github.com/yrashk/ycxx/issues/648).
    [#737](https://github.com/yrashk/ycxx/issues/737)
  - A system-linked arm64 function can destroy one initialized aggregate local on both exits from
    a direct external `void()` call. The landing pad saves the exception header, runs the same
    destructor as the normal continuation, and resumes the original exception. The object's
    cleanup-only LSDA and frame records are checked in `unit.darwin_local_unwind`.
    [Design](docs/design/aarch64-hosted-cleanup.md) · [#781](https://github.com/yrashk/ycxx/issues/781)
  - An ordinary potentially throwing call in a selected aggregate member default requires a
    supported exceptional cleanup path when a nontrivial local is live.
    [#850](https://github.com/yrashk/ycxx/issues/850)
  - Not yet: thread-local variables, dead stripping, literal merging, branch islands beyond
    ±128 MiB, other dylibs' re-exports, incremental relinking, and import addends outside
    0..255 (`DYLD_CHAINED_IMPORT_ADDEND`). `__LLVM` (embedded bitcode), `__LD` and debug sections
    are dropped.
- Objects from ycxx, Clang or GCC are linked by `ycxx cxx a.o b.o -o out`, with libSystem's
  exports read from the SDK (`$SDKROOT`, else `xcrun --show-sdk-path`). There is no separate
  linker binary (DECISIONS §4.7: one binary, many fronts).
- `unit.link`: SHA-256 vectors, object round trips, `.tbd` parsing, diagnostics, the exports trie
  and determinism on both hosts. On arm64 macOS it also runs hand-encoded programs (exit code,
  calls between objects, data, GOT and pointer relocations, `puts` from libSystem), checks
  `codesign -v`, and links a C object compiled by the bootstrap compiler.

## Lowering and AArch64 code generation, α subset (#26, #28, 2026-11-18)

The α-slice subset (epic #39): C-like functions from L1 to running arm64 macOS code. #26 and #28
stay open for the rest.

- L2 (`src/ir/l2.cppm`, module `ycxx.ir.l2`): values `i1`/`i8`/`i16`/`i32`/`i64`/`ptr`, the
  `!state` chain, blocks with arguments, an op table built by reflection (constants, `symbol`,
  `alloca`, integer arithmetic with poison flags, `icmp`, `select`, width changes, `ptradd`,
  `load`/`store` with an alias tag, `call`/`call_ind` with a mask of signed arguments and the
  number of arguments the callee's ellipsis matches (L1's `call` carries it too, #189), `br`,
  `cond_br`, `ret`, `unreachable`), a verifier, a printer and an L1 origin map per node.
- `lower` (`src/ir/lower.cppm`, `ycxx.ir.lower`): L1 to L2 over a `layout_source` concept (type
  sizes, path offsets, constant values), a stand-in for the `layout` query until #33. Access paths
  and pointer arithmetic become byte offsets, `alloc` becomes `alloca` (erroneous and zero
  initialization store zero), lifetime and unsequenced markers are dropped, `consteval_br` takes
  its runtime edge, `bool` is `i1` as a value and a byte in memory, and narrow results are returned
  extended to 32 bits. Anything outside the subset (aggregates, `copy`, `switch`, EH, coroutines,
  dynamic storage, virtual calls) is an error naming the node.
  An address constant (a string literal's, as function_ir gives it) becomes `symbol` plus an offset.
- `ycxx.target.aarch64` (`src/target/aarch64/`): the target description is data.
  - `isa.cppm`: instruction forms with their fixed bits, width rule and operand fields, read by one
    encoder.
  - `aarch64.patterns`: the pattern table in the rule language, embedded with `#embed`, parsed
    by a `constexpr` parser and checked by a `static_assert`.
  - `abi.cppm`: register classes and the calling convention (AAPCS64 with Apple's natural packing
    of stack arguments, 32-bit extension of narrow register arguments, and every argument a
    callee's ellipsis matches on the stack in an 8-byte slot of its own, #189).
  - `select.cppm`: maximal munch per block, trees cut at shared nodes, loads folded only within a
    state epoch, leaves rematerialized, edge copies for block arguments.
  - `emit.cppm`: every virtual register in a stack slot, frame layout with a frame record and
    stack probing for frames over a page,
    pseudo-form expansion, and encoding into `ycxx.obj` with BRANCH26, PAGE21/PAGEOFF12 and GOT
    relocations.
  - `compile_function` gives one function's machine code apart from any object (level 0 of
    `optimized`); `assemble` puts functions and data (cstring, rodata, data_const, data, bss) into
    one object in the order its caller gives; `codegen(unit)` does both in entity-id order.
- Tests (`tests/unit/codegen/`):
  - `unit.lower`: golden L2.
  - `unit.aarch64`, on both hosts: encodings against the Arm ARM, the pattern table, selection
    and determinism.
  - `unit.aarch64`, on arm64 macOS: generated programs linked by ycxx's linker and run (recursion,
    loops over locals and block arguments, `puts`/`putchar`, globals, a pointer to a string
    literal). A C harness compiled by GCC checks about 60 generated functions against C's own
    arithmetic and calling convention, over thousands of argument pairs. It covers narrow types,
    stack arguments both ways, function pointers and the GOT. Mutating `sdiv` or the stack packing
    makes it fail. Two variadic C callees read what generated calls pass through their ellipsis
    with `va_arg`, after named stack arguments and with none.
- Variadic definitions on Apple arm64 ([#647](https://github.com/yrashk/ycxx/issues/647)):
  `__builtin_va_list` is `char*`. Each cursor operation takes the address of the variable that
  holds it, so lowering sees a pointer to a `char*` cursor on both targets. `va_start` stores
  the address of the first 8-byte slot after the named stack arguments, at `x29 + 16` plus
  their size rounded to 8. `va_arg` loads the slot and advances the cursor by 8, `va_copy`
  copies it and `va_end` does nothing. `own/variadic/definitions` calls ycxx's definitions
  from ycxx and from a Clang peer, and lets the peer's `va_arg` read a cursor ycxx started.
  It fails if the named stack size is ignored or the cursor advances by 4. `<stdarg.h>` works
  as well as `<cstdarg>` ([#676](https://github.com/yrashk/ycxx/issues/676), see the compiler resource `<stdarg.h>` section).
- Not yet: the allocation
  validator and dependence checker (#29), SSA linear scan, list scheduling, unwind info and DWARF,
  a measured `cost_model` (#9), more than one result, frame objects aligned above 16 bytes.

## System V AMD64 frames

- x86-64 frames can be up to 16 MiB, as on AArch64. A frame over a page is allocated one page
  at a time, touching each page before RSP moves past it, so a guard page below the stack
  faults instead of being skipped. `unit.linux_stack_probe` runs a large frame on a thread
  stack with writable memory below its guard page and checks that the first fault is in the
  guard page. [#644](https://github.com/yrashk/ycxx/issues/644)

## System V AMD64 scalar integer division

- The x86-64 emitter supports L2 `sdiv`, `udiv`, `srem` and `urem` for `i32` and `i64`.
  `unit.x86_64` lowers C++ functions using `/` and `%` and runs them on Linux x86-64. It checks
  signed boundary values, unsigned high bits and the existing diagnostic for narrower L2 types.

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
  - `tokens`, `items` and `entities` are placeholders that hold only fingerprints, until #18
    and #33.
  - Shared by the engine's workers (#107): pools are append-only chunked storage that readers
    use without a lock, and each interner's fingerprint index is 64 locked shards, so equal
    content interns to one handle whichever worker is first (`ycxx.ast.store`). Handle numbers
    may differ between runs; fingerprints never do. `unit.ast_concurrent` interns 3,000 trees
    (some with lists, names and blocks longer than a storage block) from 8 threads in shuffled
    orders and compares with a single-threaded run; `unit.ast_store` covers the storage.
- **Fingerprints** (`fingerprint.cppm`): a thin adapter over `ycxx.hash` (#14); `Fp128` is
  `ycxx::Fp128`.
  - A handle's `hash_into` (`handles.cppm`, #68) appends its interner's Merkle fingerprint. It
    reaches the `Interners` bound on the thread (`ast::BindInterners`; `pipeline::Bind` binds a
    session's), and fingerprinting a handle with none bound violates a contract.
  - The engine's in-memory tables use `local_hash`, which counts a handle by its number.
  - Handles serialize by number; the persistent cache (#17) maps them through fingerprints.
  - `unit.ast_fingerprints` (mutation-checked): different interning orders, equal fingerprints.
- **Frame builder** (`builder.cppm`): drafts in the frame's arena (a `std::pmr` resource).
  `commit(value)` interns what the value reaches, bottom-up, so abandoned drafts leave nothing
  (§9.3). Lists of records that hold nodes are drafts; other lists are interned eagerly.
- **Red view** (`red.cppm`): a stack cursor with item-relative offsets and the parent chain, and
  `find(offset)` for descent.
- **Compaction** (`compact.cppm`): holders mark live values, the survivors are renumbered with
  their fingerprints kept, and holders rewrite their values with the returned `Remap`. Covers
  `ast` and `seqs`.

## Types and constants (#33, α subset)

`src/ast/`, from the [AST design](docs/design/ast.md) §5; unit test `unit.ast_types`.

- **Type kinds** (`types.cppm`): `TBuiltin` (every fundamental type of [basic.fundamental] and
  [basic.extended.fp], `meta_info`, the markers), `TPointer`, `TLRef`, `TRRef`, `TArray`,
  `TFunction` (adjusted parameters, `noexcept` as a bool constant, variadic, cv, ref-qualifier,
  language linkage) and the sugar kind `STypedef`, whose spelled type is marked `[[=canonical]]`.
- **The `types` interner** (`interner.cppm`): a kind table numbered densely across kinds, and
  `canon[]` filled at interning (§5.2). `Interners::intern_type`, `qualified` (cv dropped on
  references and functions, pushed into array elements, the canonical form interned at once) and
  the `const` reads `canonical` and `same_type`.
- **The `values` interner**: `VInt` (bool, character and integer values as 128-bit two's
  complement, normalized to the type), `VNullptr`, `VString` and `VLvalue` (added by #41), and the dependent family's shape: kinds marked
  `[[=dependent]]` (`VDepParam`, `VDepFnParam`), `Values::is_dependent`.
- **Targets** (`target.cppm`): sizes, alignments and floating formats of the fundamental types,
  `char` signedness, the character types' underlying types; `aarch64_darwin` and `x86_64_linux`.
- **What sema calls** (`typeops.cppm`): checked constructors (reference collapsing, parameter
  adjustment, `function_type`, `array_of` with the bound converted to `size_t`), predicates that
  see through sugar (`is_pointer`, `pointee_of`, …), integer conversion rank and floating-point conversion rank and
  subrank ([conv.rank]), promotions with bit-fields ([conv.prom]), the usual arithmetic
  conversions ([expr.arith.conv]), integer constants and their integral conversion
  ([conv.integral], [conv.bool]).
- Compaction keeps every type and constant, and rewrites the lists they hold.
- Later slices add enumeration and member pointer types, more dependent types and constants, and
  floating constants (`VFloat`, #336).

## Parser and sema for the α subset (#41)

`src/parse/` (`ycxx.parse`) and `src/sema/` (`ycxx.sema`); unit test `unit.sema`, own tests
`tests/own/c0`. `ycxx -fsyntax-only a.cpp` checks a file in-process and renders
`file:line:col: error:` diagnostics; `syntax` is in `tests/features/ycxx.txt`.

- **The parser analyses as it parses** (ast.md §3.2): recursive descent with one function per
  [gram] production, over one part of one item, calling sema's actions.
- **Per-item queries** (§3.6): `item_sema(item)` gives an `ItemSema` whose `decls` follow the
  skeleton's ordinals, so `decl_sema(DeclId)` is `decls[ord]`; `analyze_body(item)` gives a
  `BodySema` whose first locals are the parameters. `parse::Program` stands in for the engine
  context until #16: it memoizes the cells for one TU and answers `visible_decls(::, name, at
  item)` from the skeletons.
- **The subset:** declarations of functions and variables (`extern "C"` and linkage blocks,
  `static`, `const`, `extern`, redeclarations sharing an entity, `main`'s rules); the C0
  statements; every C0 operator with its precedence, the usual arithmetic conversions, promotions,
  value categories and lvalue-to-rvalue conversion as explicit nodes; calls; unqualified lookup in
  block and namespace scope; string literals, `const char*`, indirection, subscripts and pointer
  arithmetic. Namespace-scope initializers and array bounds are evaluated by the interpreter (ε,
  below).
- **Trees:** the node kinds `LocalRef`, `Conditional`, `While`, `Do`, `For`, `Break`, `Continue`,
  `DeclStmt`, `Declarator`, `DeclGroup`, `Assign.computation` and more `ConversionKind`s are in
  `ycxx.ast.nodes`. `VString` and `VLvalue` are in `ycxx.ast.values`.
- **Diagnostics** are values located at `(item, token offset)`; what lies beyond the subset is
  diagnosed as "not supported yet" where the grammar meets it, and members of classes and
  namespaces are not analysed.
- **Stand-in:** entities are a side table in `ycxx.sema:types` behind the AST core's
  fingerprint-only `entities` placeholder, until #33 has them.
- `tests/features/ycxx.xfail` lists the 679 tests ycxx is expected to fail (classes, templates,
  directives, …), with its first diagnostic as the reason; an XPASS fails the run, and an XFAIL
  never covers a crash. No draft example crashes or hangs ycxx.
- Not yet: overloading, classes, namespaces, templates, references, floating types, casts,
  `sizeof`, braced initialization, dynamic initialization at namespace scope, default
  arguments, block-scope statics.

## IR generation, α subset (#24, 2026-11-18)

- `ycxx.irgen` (`src/irgen/`): `function_ir(interners, FunctionInput, facts)` builds a function's
  L1 body from its analysed AST, with `FunctionDebug` beside it (a `Loc` per node, and the
  `alloc` holding each local and parameter). It covers #39's α1 and α2:
  - parameters and locals as `alloc [T, automatic, erroneous | indeterminate]`; the initializing
    store comes before `obj.begin`, and every scope exit (block end, `return`, `break`,
    `continue`) emits `obj.end` and `dealloc` in reverse order of declaration;
  - loads and stores of scalars through empty access paths; namespace-scope variables through
    `global`, and string literals as `const_` of the literal object's address, decayed by
    `ptr_member [/e0]`;
  - integer arithmetic (`nsw` on signed `+ - *` and negation, none on `<<`, `ashr` or `lshr` by
    signedness), `++`/`--` and compound assignment done in the promoted type, `iconv`, `tobool`,
    integer and pointer compares, pointer arithmetic;
  - `if`, `while`, `do`, `for`, `break` and `continue` as blocks that thread the state; `&&`,
    `||` and `?:` (prvalue and glvalue) join on a block argument;
  - calls to named functions as `call` (`not_constexpr` unless the callee is constexpr);
    falling off the end returns 0 from `main` and is `unreachable` elsewhere;
  - `useq.begin/next/end` around unsequenced operands when one writes and another touches
    memory.
  The conventions lower relies on are listed at the top of `src/irgen/gen.cppm`.
- Types, constants and declaration facts come through the `Facts` concept
  (`src/irgen/facts.cppm`). `TypeFacts` (`src/irgen/types.cppm`) answers the type and constant
  half from #33's interners and a `Target`. The declaration half (`is_constexpr`, `is_main`) and
  string literals' addresses are composed on top by the caller; the tests stand them in, since
  the interners do not hold entities or strings yet.
- `ycxx.ast.nodes` gained the kinds it reads beyond the AST core, in #41's shapes, taken verbatim
  from #41's branch (draft PR #52): `LocalRef`, `Conditional`, `While`, `Do`, `For`, `Break`,
  `Continue`, `DeclStmt`, `Declarator`, `DeclGroup`, `Assign::computation` and six conversion kinds.
  Parameters are the body's first locals.
- `unit.irgen` builds bodies with the AST builder, generates, runs `verify`, and compares the
  text form (handles named) with the expected text. It also checks that positions do not reach
  the fingerprint.
- Not yet:
  - `function_ir` and `function_debug` keyed by a context (they are keyed by unit and entity, #16);
  - `is_constexpr` over real entities (the front end reports none yet);
  - classes, arrays as objects, cleanups, EH (`invoke`), coroutines, static locals, calls
    through pointers, `switch` and `goto`. (Floating point: #336.)

    through pointers, floating point.

## The pipeline: `ycxx cxx a.cpp -o a` (#43, 2026-11-18)

The α slice end to end (epic #39): every test in `tests/own/slice` compiles, links and runs, or is
rejected at its marked locations, with `tools/test --cxx ycxx own/slice` on arm64 macOS.

- `ycxx.pipeline` (`src/pipeline/`): every stage is an engine query (#63), run by `client::cxx`
  through a `Session` (the engine, the interners and sema's world, alive together; in-process
  until the daemon, #96):
  - `:front`: `TuSema(TU)`: `PpTokens` (the region without token locations, expansion sites,
    `space_before` flags or error details; each token's file kept) and `RegionItems`, then
    `parse::Program` (item_sema, analyze_body) inside the one cell until contextual queries (#16).
    `TuFront(TU)` places its messages in files from `pp`'s full region, so an edit that changes
    no token (a comment, blank lines) stops at `PpTokens` and analysis does not run again (#947,
    slice 0).
    - Units are analysed a group at a time (#947, #978;
      `src/pipeline/groups.cpp`): each top-level item of the main file, and each run of the
      headers' top-level items, by a Program of its own (`Program::analyze_group`) that analyses
      other items only as the group asks. With specialization-heavy headers, a short run of
      main-file items shares a Program. Cache misses run on the session pool, and their results
      merge in source order. `unit_of` makes each group's `UnitPart`, and
      `finish_unit` the decisions that need every group (init functions, `inline`, noexcept and
      hidden declarations anywhere, unwind checks, virtual table ownership and RTTI).
      Peer ABI checks record the late declarations they read and merge layout uncertainty and
      ownership in source order.
    - The unit combines layout uncertainty from all groups before the native ABI checks class
      shapes and chooses base-object constructor and destructor variants.
      [#992](https://github.com/yrashk/ycxx/issues/992)
    - A Program answers the same whichever items ran first (§A1): `ensure_declared` analyses the
      earlier items that can declare an entity before a side table is read, `settle` completes a
      class reached lazily, and `may_have_virtual_layouts` is a fact of the tokens. A question
      about one specialization reads only the explicit and partial specializations that declare
      its name, and a group's END reads those from its first item on (#1704).
    - A group's analysis is reused between revisions while what it read gives the same answers
      (`GroupTrace`, kept per session): the items it read (`ItemsView`) and their position-free
      content, its by-name questions (`Program::NameProbe`, filtered by the shared
      `Program::Index` before an item is read) and their answers, the order of those items, and
      the unit's namespaces, using-directives and specialization items. The stand-in for #16's
      selection cells, as `pp::Session::cache` is for regions. Token ranges a pragma holds are
      positions: a unit with them analyses every group. Its diagnostics by byte offset, and the `TranslationUnit`: entities, function
    definitions with their analysed bodies, variable definitions with their constants.
    `tu_unit(TU)` projects the unit, so a whitespace edit stops there.
  - `:back`, per function `F`: `function_source(TU, F)` (a projection that backdates),
    `function_gen` with the projections `function_ir` and `function_debug`, `lower`, `unit` (the
    optimization unit in a content-addressed store, with which references the object defines),
    `optimized(unit, target, level)` and `codegen(F)` = `best_available` over the
    `opt_cache_snapshot` input (consulted for B ≥ 1 only); `cost_model` is a stub input. `layout`
    is three cheap queries (`layout_type`, `layout_step`, `layout_constant`). `tu(TU)` assembles
    the object in definition order (§A5.3), with string literal objects as local `l_.str.N`
    entities by first use; its value also holds the first-demand table (empty until #16). Per-unit
    cells give the per-function ones what they read of the whole unit, so an edit re-runs them
    cheaply or not at all (#938): `UnitIndexQ` (each definition's and entity's place),
    `UnitFunctionFactsQ` (its constexpr and variadic functions), `UnitShapeIndex` (`layout`'s
    class and enumeration lookup) and `SymbolAccessQ(TU, E)` (how a unit's code addresses `E`).
    On x86-64, `Lower` still reads the whole unit (#943). Errors
    (what the α subset does not support yet) are values, pointing into the item when there is a
    node.
  - `:mangle`: Itanium names for global-namespace functions over fundamental, pointer and cv types
    with substitutions; `main`, `extern "C"` functions and variables are not mangled (#64).
  - `:executable` provides `link(P)`, the query over a program's `tu` objects. The Darwin link
    also takes `.o` files and libSystem's exports from `$SDKROOT`, else `xcrun --show-sdk-path`,
    else the Command Line Tools' SDK, looked up once per process outside the engine.
  - `:runtime` (configured by CMake): what a Darwin link includes by default after its operands
    (#842): this build's `libycxx.a` and `libycxx-abi.a`, GCC's `libemutls_w.a` for emulated TLS,
    the two GCC shift helpers `___ashlti3` and `___lshrti3` for optimized `to_chars` (#1130),
    and the names libycxx's link options force undefined (`___ycxx_allocation_table_anchor`, read
    from its `ycxx-link-options`). Members are selected as for any Darwin archive, so `<cstdio>` programs
    link against libycxx's linkage marker, and libycxx-abi's `__cxa_*` come before libc++abi's.
    A trivial program grows from 17 KB to 118 KB with the allocation table. Configured runtime archives are
    read once per process while their size and modification time hold, and every program's
    inputs share the bytes (`obj::archive::input`, fingerprinted once). `-nostdlib++` links
    without the runtime. `unit.darwin_runtime`, `own/headers/cstdio.run.cpp` and
    `own/runtime/ios-base-init.run.cpp` (libycxx's `std::ios_base::Init`, through emulated TLS)
    check it.
  - `:lines`: a minimal line table for rendering (#34 owns the full one).
- `ycxx cxx` (also `ycxx -…` with an option first) takes `-o`, `-c`, `-fsyntax-only` and
  `-std=c++26`. Errors are `path:line:col: error: message` on stderr. It exits 0 on success and 1
  on any error in the input or the command line.
- `-fcheck-policies` applies YCXX Policies to ownership contracts before a syntax-only or native
  build succeeds. It composes active C++ policy fragments and declaration annotations across the
  build's source inputs by canonical types and resolved operations. It reports conflicting contracts
  with both origins. A per-body control-flow query and worklist check named affine and linear
  resources, including concrete template instances. Transfers invalidate the old owner.
  A `move_transfers<T>` entry or its class annotation authorizes a nonthrowing move. A plain
  wrapper around a move-only guard can use its implicit move constructor.
  Conditional and comma initializers preserve the selected path and evaluate earlier operands
  in order for named owners, non-overlapping fields, and fixed-array elements.
  The 1,280-resource fixture composes four source fragments and checks that an
  equivalent reflection edit reuses the caller's proof while a conflicting edit reports both origins.
  Local const references can borrow named owners or bind to another local shared reference.
  Each reference protects the original owner through its last reachable use.
  The checker rejects consuming or transferring an owner before a future use and rejects
  exposing the borrow through an address, capture, or unregistered operation.
  A `verified_observes` contract checks the observer's scalar reads and forwarding of the
  original shared borrow to other verified observers. An observer can return a copied
  arithmetic or enum value. Checked ELF and Mach-O objects carry
  observation requirements and guarantees through each forwarding function. The checked link
  matches them to unique strong definitions in explicit input objects. One forwarded observation
  can also require a verified effect bound from the selected provider.
  Local arrays and aggregates track registered owners by nested field and literal-index
  paths such as `queue.groups[1].tasks[0]`. Initializing, borrowing, transferring, and
  consuming a subobject follow the same checks as individual named resources.
  `tracks_variant<Choice>` checks the active alternative of a package owner variant. The
  `holds` tests refine the branch, shared borrows delay extraction, and extraction transfers
  the obligation to the destination. Checked executables run natively on x86-64 Linux and
  arm64 macOS with or without runtime guards.
  [Variant conformance](tests/unit/pipeline/policies_variants.py)
  The standalone C++20 package also provides move-only issuer-scoped capability tokens with
  rights constraints and runtime scope checks. On Linux x86-64, a checked YCXX object and a
  hosted GCC or Clang object exchange the token by value in either direction.
  [ABI support #661](https://github.com/yrashk/ycxx/issues/661) · [Dependent type friends #689](https://github.com/yrashk/ycxx/issues/689)
  Finite effect bounds from `<ycxx/effects.hpp>` check every direct callee of an annotated
  function, including calls in default arguments and recursive calls. Typed declarations
  and C++26 annotations share the registry. Bounds use canonical sets of nominal tags.
  Each verified body must have a unique supported definition among the checked source
  inputs or in the objects at the final checked link. The link verifies all requirements
  through intervening providers and reports inconsistent trusted declarations it encounters.
  The checker rejects indirect calls and unmodeled implicit operations.
  An `at_most<Tag, N>` entry adds a static usage grade for one tag. The check adds
  sequential costs, takes the maximum across alternatives, and counts terminating loop paths.
  Counted `int` and `unsigned` loops use the converted integer values of their endpoints.
  Explicit object links carry graded contracts. The C++20 package also offers scope-local runtime budgets
  and explicit scoped handlers. For a local handler bound to a named function, YCXX verifies
  the constructor and forwarding method, prevents the handler from escaping, and uses the
  selected function's outward effects and grade. The checked link verifies that function's
  contract across explicit objects. Stateful callable handlers can use trusted adapters.
  [YCXX Policies #660](https://github.com/yrashk/ycxx/issues/660)
  Execution leaving the scope of a live transaction or reaching a possible unwind is diagnosed.
  See [the protocols and examples](docs/design/policies-ownership.md).
- `tests/features/ycxx.txt` lists `syntax` and `run darwin-arm64`: a feature can be limited to
  hosts. CI's macOS job runs the slice before its tier, with `--no-unsupported`.
- `unit.pipeline`: the line table, linkage names, and the back half from hand-built analysed
  bodies (`compile`, `tu(TU)` in an engine of its own), linked and run on arm64 macOS.
- `unit.pipeline_incremental`: a second build executes nothing; a new revision through an
  unrelated input validates everything and executes nothing; a whitespace edit executes nothing
  past the front end; a body edit runs that function's cells once each, `tu` and the link, and no
  other function's; incremental equals cold after each edit, also when the warm session numbers
  entities differently (mutating `tu` to order by handle makes it fail).
- Not yet: `ycxx cxx` through the daemon by default (#120), `#include` (#65), optimization
  levels and the other common options (`-O` is rejected, #81).

## x86-64 ELF objects and executables (2026-11-19)

- `--target=x86_64-pc-linux-gnu` (also `x86_64-linux-gnu`) selects Linux preprocessing,
  layout and code generation. The in-process driver and daemon use the same request parser.
  It defaults to Linux on x86-64 Linux builds and to `arm64-apple-darwin` on arm64 Darwin builds.
  An explicit `--target` overrides the default. Builds with other configurations require one.
  A native Linux executable link requires `-nostdlib`. [#651](https://github.com/yrashk/ycxx/issues/651)
- The scalar System V AMD64 backend in `src/target/x86_64/` supports `bool` and 8-, 16-, 32- and 64-bit
  integer and enum parameters and results, plus pointers, references and void results. It handles
  direct and indirect calls, stack arguments and RIP-relative data references within a unit.
  Narrow register arguments and results are extended to 32 bits when sent for interoperability.
  Incoming values use their declared width in registers and stack slots (AMD64 psABI §3.2.3).
  A `bool` uses the byte value 0 or 1.
  Direct and function-pointer variadic calls use the same registers and stack slots for promoted
  32- and 64-bit integers and pointers. INTEGER-only variadic calls set AL to zero even with
  an empty ellipsis.
  Indirect variadic calls keep their target in R11.
  Ordinary free variadic definitions with scalar INTEGER and SSE signatures support `<cstdarg>`.
  `va_start` includes zero fixed parameters and captures the incoming GP and XMM registers in
  each activation, before its body can make calls. Its cursor follows AMD64 psABI §3.5.7 and can
  be passed to a separately compiled consumer. Native `va_arg` retrieves `int`, `long`,
  `long long`, their unsigned counterparts, ordinary pointers and `double`. Integer and SSE
  cursors advance independently in the register save area and share the overflow stack cursor.
  `va_copy` gives each cursor independent positions and `va_end` evaluates its operand.
  `unit.linux_variadic_definitions` exercises native execution, host peers, and standalone and
  daemon compilation. Calls through pointers pass `float` and `double` arguments and results
  as direct calls do; through a variadic pointer, AL counts the vector registers used
  (`unit.linux_scalar_sse`, #196). Aggregate variadic signatures and retrieval remain in #196.
  [#417](https://github.com/yrashk/ycxx/issues/417), [#412](https://github.com/yrashk/ycxx/issues/412),
  [#399](https://github.com/yrashk/ycxx/issues/399),
  [#196](https://github.com/yrashk/ycxx/issues/196)
- Naturally aligned 32-bit GNU atomics on Linux x86-64 use stateful L1 and L2 nodes
  for loads, stores, exchange, compare-exchange, additive and bitwise updates, and
  thread fences. The target defines the six GNU order macros and supports the null
  alignment lock-free query for sizes 3 and 4. See [Extensions](docs/EXTENSIONS.md#gnu-atomics-on-linux-x86-64).
  [#1293](https://github.com/yrashk/ycxx/issues/1293) · [#328](https://github.com/yrashk/ycxx/issues/328)
- Fixed-prototype definitions and direct calls also transport `float` and `double` constants,
  loads, stores and results. INTEGER registers and XMM registers are assigned independently.
  `unit.linux_scalar_sse` exchanges objects with a native peer, checks exact bits, mixed spills
  and INTEGER-record rollback, and runs the pinned `<cfloat>` probe through the native linker
  in both cold and daemon builds.
  [Design](docs/design/linux-scalar-floating-point.md) · [#509](https://github.com/yrashk/ycxx/issues/509)
- Direct variadic calls transport named `float` and `double`, promoted `double` in the ellipsis,
  and scalar floating results. AL counts the XMM registers assigned to arguments, including
  named arguments with an empty ellipsis. The emitter uses SSE2 for binary32-to-binary64 widening
  in argument promotion and other expressions.
  [Design](docs/design/linux-floating-variadic-calls.md) · [#399](https://github.com/yrashk/ycxx/issues/399)
- Runtime addition, subtraction, multiplication and division use scalar SSE at the binary32 or
  binary64 L2 result width and under the caller's MXCSR controls. Unary negation flips the sign
  bit without changing the payload or status flags.
  [Design](docs/design/linux-scalar-floating-point.md#runtime-arithmetic)
- Binary32 and binary64 comparisons use the scalar SSE unordered comparison instructions.
  Equality and the four relational operators are false for unordered operands. Inequality and
  conversion of NaN to `bool` are true. Both signed zeros convert to false. Strict exception
  status and x87 values remain [#201](https://github.com/yrashk/ycxx/issues/201), [#645](https://github.com/yrashk/ycxx/issues/645).
  [Design](docs/design/linux-scalar-floating-point.md#runtime-comparisons)
- Conversions between binary32 or binary64 and 32- or 64-bit integers use the SSE2 conversion
  instructions; narrower integers and `bool` widen to 32 bits first. Integer-to-floating
  conversions round under the caller's MXCSR. An unsigned 64-bit value with its top bit set is
  halved with the shifted-out bit kept as a sticky low bit, converted and doubled. A floating
  value at or above 2^63 converts to unsigned 64 bits through the conversion of x − 2^63.
  Binary64 to binary32 rounds under MXCSR, and `fabs` clears the sign bit. `unit.linux_scalar_sse`
  compares exact bits with a host compiler's conversions at `-O0` and `-O2` under all four
  rounding directions. [#643](https://github.com/yrashk/ycxx/issues/643)
- Fixed-prototype calls support nonempty naturally aligned flat records with integral, ordinary
  object-pointer or object lvalue-reference fields. A record must be trivial for calls and occupy
  up to 16 bytes with alignment up to eight. The AMD64 INTEGER eightbytes follow from the laid-out
  fields. If an entire argument cannot
  fit in the remaining registers, it goes by value on the stack and later arguments can use
  the remaining registers. One- and two-eightbyte results use RAX and RDX. The mixed-object
  tests exercise both directions and check by-value object identity. Source packing and
  alignment directives with uncertain layout exclude the record from this by-value ABI.
  Member functions (with `this` first), closure call operators, function template
  specializations and calls through pointers use the same placement as ordinary functions
  ([#640](https://github.com/yrashk/ycxx/issues/640)).
  Indirect calls preserve the last by-value argument with or without a class result destination
  ([#693](https://github.com/yrashk/ycxx/issues/693)).
  A class result in memory, of more than two eightbytes or not trivial for calls, goes
  through a hidden pointer that takes RDI ahead of `this` and comes back in RAX. A class
  argument not trivial for calls passes by the address of the caller's object. A naturally laid
  out one-byte empty class trivial for calls uses no register or stack slot in a fixed direct
  call. The callee still has a distinct parameter object. A result with the same layout,
  including a captureless closure, carries zero ABI value components in a fixed direct call.
  The callee evaluates the return operand and the caller keeps the result object in its own
  storage when used. Other empty records and trivial stack arguments remain tracked in
  [#641](https://github.com/yrashk/ycxx/issues/641). SSE records are tracked in
  [#642](https://github.com/yrashk/ycxx/issues/642). Records with source layout directives
  native layout does not apply stay out of memory passing, including records that contain
  those layouts as fields, array elements, or bases. A wrapper whose member makes its implicit
  destructor nontrivial uses the indirect result convention even when its copy and move traits
  are unknown. [#661](https://github.com/yrashk/ycxx/issues/661)
  [Design](docs/design/linux-integer-aggregates.md) · [#440](https://github.com/yrashk/ycxx/issues/440) · [#988](https://github.com/yrashk/ycxx/issues/988)
- `-c` writes deterministic ELF64 relocatable objects through `src/obj/elf.cppm`. The reader
  accepts the supported x86-64 `ET_REL` subset emitted by ycxx. The native ELF linker consumes
  the same in-memory objects, resolves absolute and PC-relative relocations, and writes a static
  `ET_EXEC` image with load segments and a non-executable stack.
- A hosted peer's exception can unwind through cleanup-free ycxx functions on x86-64 Linux.
  The compiler certifies ordinary strong definitions with fixed scalar parameters using all
  resolved declarations and the typed body. The proof visits selected default member initializers
  in aggregates with scalar fields and trivial destruction. It checks fixed scalar prototypes
  and the typed operands of indirect calls.
  [#849](https://github.com/yrashk/ycxx/issues/849) · [#851](https://github.com/yrashk/ycxx/issues/851)
  Returning a nonempty flat scalar aggregate with trivial destruction and call semantics uses the
  same proof for a braced result or an implicit trivial copy. The target's existing register or
  indirect result convention still applies. [#853](https://github.com/yrashk/ycxx/issues/853)
  It tracks written exception specifications across redeclarations and records the AMD64 frame
  state at the actual prologue and epilogue offsets.
  ELF `.eh_frame` records use PC-relative references to local code anchors. The native
  freestanding linker retains these records in the read-only segment. The hosted linker
  supplies unwind discovery for the peer who throws and catches the exception.
  `unit.linux_cleanup_free_unwind` checks mixed hosted execution, omitted frames, and native
  source, staged, and daemon link parity. Standalone unwind discovery and broader handler and
  exceptional-cleanup support remain under #89.
  [#572](https://github.com/yrashk/ycxx/issues/572) · [#89](https://github.com/yrashk/ycxx/issues/89)
  · [#558](https://github.com/yrashk/ycxx/issues/558)
- A host-linked ordinary void function can destroy one initialized aggregate local with a
  nonthrowing destructor when its subsequent direct external `void()` call throws. The normal
  continuation destroys it once as well. The cleanup-only landing pad resumes the original
  exception; the mixed-object test checks both paths and the peer's typed handler.
  [Design](docs/design/linux-hosted-catch-all.md#hosted-local-cleanup-663) · [#663](https://github.com/yrashk/ycxx/issues/663)
- A hosted nondelegating no-argument union constructor can initialize one scalar aggregate
  variant with a nonthrowing destructor, then run one external `void()` call. If that call
  throws, the active member is destroyed and the original exception resumes. If an optional
  external `int()` in the initializer throws first, neither destructor runs. A caller that
  only constructs one such automatic union in the same unit supplies a cleanup-free frame.
  [Design](docs/design/linux-hosted-catch-all.md#hosted-union-constructor-cleanup-738)
  · [#738](https://github.com/yrashk/ycxx/issues/738) · [Darwin parity #648](https://github.com/yrashk/ycxx/issues/648)
- Explicit native static links keep the first input ELF COMDAT group for each signature. The reader
  validates group membership, including relocation sections, and selection precedes startup and
  layout. `unit.linux_elf_comdat` checks whole-group selection, retained references, startup and
  same-path daemon changes to the signature. Broader group emission, archive and dynamic linking stay in
  [#422](https://github.com/yrashk/ycxx/issues/422). [Design](docs/design/linux-elf-comdat-input.md)
- The ELF writer can group one `.data.rel.ro` section and its generated relocation section by a
  weak data symbol. The relocation section holds named nonlocal `R_X86_64_64` references.
  Relocation-free `.rodata` groups still supply RTTI names.
  [#422](https://github.com/yrashk/ycxx/issues/422) · [Design](docs/design/linux-elf-comdat-input.md)
- ELF function definitions carry `STT_FUNC` and their emitted code size. The reader retains
  both for supported system relocatables and accepts zero as an unknown size. A function must
  start inside executable code, and its declared extent must fit in its section. The object
  fingerprint includes this metadata.
  [#428](https://github.com/yrashk/ycxx/issues/428) · [#391](https://github.com/yrashk/ycxx/issues/391)
- Runtime addresses of default-visible Linux data and ordinary functions use
  `R_X86_64_REX_GOTPCRELX`, including declarations defined in another object. The native static
  linker checks and relaxes the RIP-relative MOV to LEA after resolving the symbol. System linkers
  can preserve GOT binding. Static pointer initializers use absolute relocations with their byte
  addends. `unit.linux_compile` checks data identity, offsets, writes, BSS, and preemption.
  `unit.linux_function_addresses` checks function identity, indirect calls, archive extraction,
  weak definition precedence, and system-linker preemption and aliases. `unit.elf_link` checks
  malformed instructions, overlapping relocations and displacement range. Native dynamic addresses
  follow [#390](https://github.com/yrashk/ycxx/issues/390).
  [Design](docs/design/linux-external-data-addresses.md) · [#414](https://github.com/yrashk/ycxx/issues/414)
  · [#407](https://github.com/yrashk/ycxx/issues/407)
- Static x86-64 ELF links resolve `R_X86_64_GOTPCREL` through read-only GOT slots. Canonical
  `R_X86_64_REX_GOTPCRELX` loads for unresolved weak symbols use the same slots with value zero.
  [#979](https://github.com/yrashk/ycxx/issues/979) · [Design](docs/design/linux-static-got.md)
- Simple external inline function definitions use weak ELF symbols. The unit combines inline
  flags from reachable declarations, including `constexpr` functions and ordinary members
  defined in their class. Calls and statically initialized pointers in different units resolve
  to one definition. `unit.linux_inline` checks symbol bindings, address identity, archive
  extraction, and both operand orders with the native and host linkers.
  [Design](docs/design/linux-inline-functions.md) · [#422](https://github.com/yrashk/ycxx/issues/422)
- Explicit GNU `visibility("hidden")` follows ordinary external namespace-scope functions across
  redeclarations. On x86-64 ELF, inline definitions combine `STB_WEAK` with `STV_HIDDEN`.
  Ordinary non-inline definitions and referenced declarations combine their symbol binding
  with `STV_HIDDEN`. `unit.linux_inline` checks both scoped attribute spellings, symbol metadata,
  and cross-unit address identity with the native and host linkers in both object orders.
  [#602](https://github.com/yrashk/ycxx/issues/602)
- On Linux x86-64, GNU `weak` on an ordinary external namespace-scope function declaration
  gives its non-inline definition `STB_WEAK`. This includes qualified definitions such as
  libycxx's `std::is_debugger_present`. The ELF linker selects a strong definition in either
  input order and diagnoses duplicate strong definitions. Explicit GNU visibility can
  independently give the symbol `STV_HIDDEN`. On arm64 Mach-O the same definitions are weak
  external, or weak private external when hidden, and both ycxx's linker and the system linker
  keep a strong definition over them (`unit.darwin_weak_visibility`).
  [Design](docs/design/linux-weak-functions.md) · [#937](https://github.com/yrashk/ycxx/issues/937)
  · [#1085](https://github.com/yrashk/ycxx/issues/1085)
- A GNU `weak` declaration of an ordinary external non-inline object gives its undefined ELF
  symbol `STB_WEAK` on Linux x86-64. The pinned allocator's marker address comparison uses the
  ordinary GOT address and remains a runtime decision.
  [#984](https://github.com/yrashk/ycxx/issues/984) · [Design](docs/design/linux-weak-object-references.md)
- On Linux x86-64, explicit GNU hidden visibility on any declaration of an ordinary external
  non-inline namespace-scope variable sets `STV_HIDDEN` on its ELF symbol.
  This includes the C-linkage marker `__ycxx_linkage_static_v1` from the pinned libycxx runtime.
  On arm64 Mach-O the symbol is private external.
  [#936](https://github.com/yrashk/ycxx/issues/936) · [#1085](https://github.com/yrashk/ycxx/issues/1085)
- On Linux x86-64, a namespace-scope `asm((expression));` checks `size()` and `data()` in one
  constant evaluation and accepts a single `.hidden SYMBOL` directive with an optional final
  newline. The assembler sets `STV_HIDDEN` on the matching ordinary ELF symbol while preserving
  its binding and relocations.
  [Design](docs/design/linux-namespace-constant-asm.md) · [#974](https://github.com/yrashk/ycxx/issues/974)
- On Linux x86-64, ordinary external non-inline function declarations can inherit the
  nearest explicit GNU `hidden` or `default` visibility from a namespace definition.
  The canonical declaration carries consistent visibility to its unannotated qualified
  definition and to reference-only objects. An explicit function attribute takes
  precedence. Mixed inherited defaults retain the direct attribute rules. arm64 Mach-O
  follows the same rules, with hidden as private external.
  [Design](docs/design/linux-namespace-visibility.md) ·
  [#940](https://github.com/yrashk/ycxx/issues/940) · [#1085](https://github.com/yrashk/ycxx/issues/1085)
- Constant-initialized and zero-initialized external inline namespace variables use weak ELF
  definitions when their initial values need no relocations. This includes explicitly inline
  full variable-template specializations with builtin type arguments. Static addresses and
  writes in multiple units resolve to the same object. The two-unit `<cstddef>` probe links
  through the pinned GNU `libycxx.a`. Group emission and dynamic inline initialization remain
  tracked separately. [Design](docs/design/linux-inline-variables.md)
  · [#432](https://github.com/yrashk/ycxx/issues/432) · [#422](https://github.com/yrashk/ycxx/issues/422)
  · [#368](https://github.com/yrashk/ycxx/issues/368)
- The ELF reader accepts the GNU-built libycxx static linkage-marker object. It reads sized
  object symbols and local section symbols, preserves the marker's hidden binding, and maps
  `.data.rel.ro.local` to the existing read-only output for relocated pointers. It validates and
  discards file symbols, ordinary nonallocated DWARF and comment sections, and an empty
  non-executable GNU stack marker. [#419](https://github.com/yrashk/ycxx/issues/419)
- ELF symbol names and COMDAT signatures can use shared suffixes and repeated string-table
  indices. Normalized names share immutable table bytes across object copies and moves, and
  equality and fingerprints follow their spelling. Invalid offsets and missing boundary NULs
  are diagnosed. [Design](docs/design/elf-symbol-names.md) · [#484](https://github.com/yrashk/ycxx/issues/484)
- `ycxx cxx --target=x86_64-pc-linux-gnu -nostdlib a.cpp b.cpp -o a` compiles and links C++
  sources in one invocation. Either source can also be compiled separately with `-c` and passed
  as a `.o` input. Links limited to sources and disk objects place compiled sources first. Each
  group follows command-line order. When the program supplies an explicit `_start`, that symbol
  is the entry. Otherwise ycxx's startup calls `int main()` or `int main(int, char**)` and exits
  with the returned value. The link query checks a source function's ABI when an asm label gives it the symbol
  `main`. A `main` supplied in an ELF object must already use one of those signatures.
- A static link can include explicit `.a` operands created by `ycxx archive` or system `ar`.
  It follows operand order across sources, objects and archives. Each archive supplies the earliest physical member
  defining an unresolved nonweak symbol, repeating until no eligible member remains. Each
  repeated operand searches the archive again at its new position. The driver snapshots the
  archive and the link query validates its GNU index and reads only the selected ELF members.
  This path requires a source or ordinary object and supports regular, indexed GNU archives
  and empty archives. Other archive formats and combining archives with shared objects remain
  part of [#388](https://github.com/yrashk/ycxx/issues/388).
- Static x86-64 ELF links validate the canonical GNU property note for `FEATURE_1_AND` with
  IBT and SHSTK bits. The linker intersects the masks of ordinary objects and selected archive
  members, treating an unmarked object as zero. A zero intersection produces an executable
  without a GNU property note or segment. A nonzero intersection is diagnosed.
  [#1367](https://github.com/yrashk/ycxx/issues/1367) · [#1376](https://github.com/yrashk/ycxx/issues/1376)
- An arm64 macOS link can include explicit Darwin `.a` operands (BSD `#1/<n>` names and a
  `__.SYMDEF`, `__.SYMDEF SORTED` or `_64` ranlib index first), with the system linker's
  selection rules: every ordinary object comes first, an archive serves names referenced
  anywhere on the command line, weak references and tentative definitions select members, and
  the first archive in operand order that lists a name supplies it, before the dylibs do.
  Members are decoded only when selected. `unit.darwin_archive` checks each rule against the
  system linker, malformed and 64-bit indexes, and daemon equality and same-path replacement;
  `unit.darwin_inline` links an archive member with both linkers.
  [Design](docs/design/darwin-static-archives.md) · [#842](https://github.com/yrashk/ycxx/issues/842)
- On both targets, the driver rereads explicit archive operands at request ingress and
  fingerprints their bytes. This covers replacements with unchanged size and mtime.
  Driver-appended Darwin runtime archives can reuse their process-local snapshots.
  [#951](https://github.com/yrashk/ycxx/issues/951)
- `ycxx archive -o lib.a [--] [object ...]` creates or replaces a regular GNU archive in process.
  It validates each member with the existing ELF object reader, copies its bytes, and indexes
  named nonlocal definitions, including weak and hidden definitions. Member basenames and
  duplicate definitions follow operand order. The format uses a 32-bit GNU index, deterministic
  metadata, and an optional long-name table. An empty input list writes a zero-entry index.
  The command stages replacement beside the output after validating all inputs. Mach-O archive
  production remains part of [#389](https://github.com/yrashk/ycxx/issues/389). The supported
  external ELF object subset is tracked in [#419](https://github.com/yrashk/ycxx/issues/419).
- `-nostdlib --dynamic-linker=/absolute/target/loader` with explicit shared objects writes a
  fixed-address, sectionless `ET_EXEC`. Libraries need a slash-free `DT_SONAME`, GNU or SysV hash
  metadata, and no loader dependencies. Provider discovery uses GNU hash on dual-table libraries
  and works without input section headers. The linker checks direct calls against their
  default-visible, unversioned global functions. One RX thunk and one eager `R_X86_64_GLOB_DAT`
  relocation in RW storage serve each imported name. The loader finds libraries by SONAME,
  binds imported names, and runs provider initializers. Startup runs the executable's
  initializers, calls `main`, and calls the loader's termination callback before exiting with
  `main`'s result. The interpreter
  path and ordered shared inputs, including provider content fingerprints, enter the `Link`
  query. Target CRT selection, versioned imports, provider dependencies and library discovery
  remain part of [#390](https://github.com/yrashk/ycxx/issues/390).
- On Linux x86-64, `-shared -nostdlib` produces a leaf `ET_DYN` with a SONAME, SysV dynamic hash,
  and mapped dynamic symbols. The loader uses the program headers, and native and system linkers
  can consume the library. Only strong, default-visible functions are exported. References to
  local and hidden definitions use position-independent relocations. Source inputs require
  `-fPIC` or `-fpic`. The resident link key includes the SONAME. Loader-time fixups, initializers,
  imports and interposable references remain in [#391](https://github.com/yrashk/ycxx/issues/391).
- On arm64 macOS, `-shared` or `-dynamiclib` with `-nostdlib` writes a signed leaf `MH_DYLIB`.
  The install name is the output basename unless overridden by `-Wl,-install_name,<name>`.
  Source inputs require `-fPIC` or `-fpic`. Strong functions can use direct local and hidden
  references and immutable data. The platform loader and rule worker can consume the image.
  [Design](docs/design/darwin-shared-output.md) · [#393](https://github.com/yrashk/ycxx/issues/393)
- arm64 Darwin executable links accept explicit leaf `.dylib` paths. The linker
  reads strong function exports and uses the provider's install name and version
  fields for its load command. Library contents participate in the resident link key.
  [Design](docs/design/darwin-dylib-input.md) · [#392](https://github.com/yrashk/ycxx/issues/392)
- ELF objects record namespace-scope initializer functions in `.init_array`, with 8-byte slots
  and `R_X86_64_64` relocations. The linker places arrays consecutively in input order and checks
  each slot resolves into executable input bytes. Generated startup calls the functions before
  `main` ([basic.start.dynamic]). A custom `_start` with a nonempty array is a link error.
  [#387](https://github.com/yrashk/ycxx/issues/387)
- ELF code generation diagnoses namespace-scope objects that require destruction at exit, at
  their declaration. Syntax-only analysis accepts them. [#400](https://github.com/yrashk/ycxx/issues/400)
- Linux x86-64 CI runs `unit.linux_compile`, which links ycxx-produced objects with native C
  callers and variadic callees. It checks namespace initialization, default promotions, register
  and stack arguments, stack alignment, pointer identity and the incoming AL for empty and
  nonempty ellipses.
  `unit.linux_executable` (`tests/unit/pipeline/linux_executable.py`) runs
  source-only, object-only and mixed-input executables and checks exit status, `argc` and `argv`,
  diagnostics, and cold and daemon output. The linker and startup also have native CTest
  coverage, including execution of an explicit `_start`. `unit.linux_dynamic` links and runs a
  libc-free provider built by the host C compiler. It checks the ELF layout, argument passing,
  runtime rebinding, initialization and finalization, and reproducibility and invalidation in
  the daemon. `unit.linux_archive` tests system `ar` inputs, extraction order, symbol binding,
  malformed archives, and output parity and same-path replacement in the daemon.
  `unit.linux_archive_output` checks byte layout, deterministic member metadata, atomic
  replacement, and native and external archive consumption. It runs the system `ar`, `nm` and
  linker and also exercises `llvm-ar`, `llvm-nm` and `ld.lld` when available.

## The resident daemon (#96, #97, 2026-11-18)

`ycxx daemon` serves `ycxx cxx`, per `docs/design/daemon.md`, by default (#139):
`YCXX_NO_DAEMON=1`, `--no-daemon` or `YCXX_DAEMON=0` turn it off, and any failure to use it falls
back to running the same handler in-process. Requests run in it at once. The test driver runs
ycxx in-process unless `YCXX_TEST_DAEMON=1`; the daemon's own tests and the `.incr` suite start
private daemons.

- `ycxx.compile` (`src/client/compile.cppm`): the c++-compatible request handler, which was the
  body of `client::cxx`. It takes argv, cwd, the environment table and the client's clock. It
  returns stdout, stderr, the exit status and the output bytes, which the client writes with
  `pipeline::write_file`. Files are read through a `pp::Host`, and the units are built in a
  `pipeline::Session` (#63).
- The session runs on a worker pool, in-process and in the daemon alike (#139):
  `compile::threads()` reads `YCXX_THREADS` (default: the hardware's concurrency) and
  `YCXX_CHAOS_SEED`; the daemon takes them from the client that starts it and reports `threads`
  in `ctl status`. Workers and the daemon's request threads have 256 MiB stacks (address space),
  above a main thread's 8 MiB (#178, #187).
- Nesting too deep for the stack (#187): `ycxx.stack::deep()` says whether more than half of the
  thread's stack is in use (pthreads, a partition per platform). The parser asks at
  `cast_expression` (parentheses, casts), `unary_expression` (`++`, `--`, `sizeof`),
  `braced_init_list` and `statement` (blocks), and at the
  first deep one diagnoses "nested too deeply to analyse" and throws `parse::TooDeep`, which the
  item's or body's analysis catches: the item fails, the unit has an error, the daemon lives.
  Instantiations nest through those parses. `engine::Pool` takes a worker stack size (default
  256 MiB); `unit.pipeline_incremental`'s `too_deep` runs 20,000-deep parentheses, blocks,
  increments, `sizeof`s and braced lists on 1 MiB workers (without the guards: SIGBUS). Not yet:
  declarators, template-ids, deep trees from flat chains, the passes after the parse, and a
  diagnostic that does not depend on the stack's size (#260). The time to get that deep is
  superlinear (#258).
- Concurrent requests (#139, daemon design §4): a unit, a program and a request are named by what
  they are (`Session::unit`, `program`, `request`: path, working directory, configuration, object;
  units, objects, name), numbered by a registry whose values provide their inputs, so admitting a
  request sets nothing and needs no quiescence. A request is one root, `Build` (front ends, then
  objects or the link), spawned into the engine's `Epochs` by `Session::run`, so a batch that
  stops it restarts the whole root at cell level and the answer is of one revision. Batches,
  editor buffers and the admission quantum are the daemon's edit strand; a batch applies through
  `Session::apply` before a request and, once the quantum allows, while requests run. A hang-up
  cancels the request's ticket. `ctl status` reports `peak_running`.
- The daemon keeps one `Session` for its life (#120). Its `pp::FileContent` cells are provided
  from the file table, with the toolchain and sysroot as `high` durability. A batch sets the
  provided entries it changed, at quiescence. A repeated request executes nothing
  (`last_executed: 0` in `ycxx ctl status`), and a header edit re-runs only what read the header.
- `ycxx.daemon` (`src/daemon/`):
  - `:scope`: the root is `YCXX_ROOT`, or the nearest `.ycxx` (with `root =`) or version-control
    marker; submodules are skipped, worktrees counted, and the fallback is the home scope. The
    runtime directory is per user and 0700 (`YCXX_RUNTIME_DIR`). The socket is
    `<scope>-<build>.sock`, with its `sun_path` length checked. The build hash is the binary's
    file identity.
  - `:protocol`: u32 length, u16 kind, and a body by `ycxx.engine:serial`. The `hello`/`welcome`
    handshake checks the build hash, the schema hash (by reflection) and the uid. Peer uids are
    checked; malformed frames close the connection.
  - `:connect`: connect; else take the start lock, connect again, then spawn a detached daemon
    (setsid, double fork, descriptors closed, log in the runtime directory) and wait for its
    readiness byte.
  - `:server`: one thread per connection, with `compile`, `status`, `shutdown` and `lsp` (an LSP
    session, below). It holds a lifetime lock with a 10 s wait and unlinks a stale socket. It exits after
    30 minutes idle (`--idle-exit`, `YCXX_DAEMON_IDLE_EXIT`), or on shutdown or SIGTERM once
    requests finish. A hang-up stops the request before its next translation unit. A request
    whose configuration environment differs from the daemon's is refused, and the client runs it
    in-process.
  - `:files`: the file table. Keys are absolute paths (relative ones against the request's cwd),
    with real paths and a reverse index for symlinks, and negative entries. Entries change only
    between epochs. A batch re-reads every entry a dirty path reaches, by path or by a directory
    above it, and drops the rest. The rescan is the fallback.
  - `:events`, `:watch`: dirty paths and shared cookies. On macOS, one recursive FSEvents stream,
    recreated from the event id taken before the first read when a directory outside it appears.
    On Linux, an inotify watch per directory, added before the read, with `max_user_watches`
    overflow stat-checked. A rescan happens on overflow, cookie timeout, watcher failure, and
    under `YCXX_NO_WATCH=1`.
  - `:admission`: the progress quantum (50 ms floor, doubling without progress).
- `ycxx cxx` falls back to in-process on any failure, and retries a lost daemon once.
  `YCXX_DAEMON=require` or `--daemon=require` makes a failure an error (the tests' hook);
  `YCXX_NO_DAEMON=1` or `--no-daemon` turns the daemon off. `-v` says where the request was
  answered, and `@file` response files are expanded by the client. `ycxx ctl status|shutdown`.
- `pp::configure(Config, Host&)` (for `-E`): a session with the same host and configuration is kept, so its
  region cache is reused between requests.
- Tests:
  - `unit.daemon`: framing and malformed frames, root discovery, the quantum under simulated
    time, file-table batches, and the real watcher with cookie sync.
  - `daemon.compare` (`tests/daemon/compare.py`): `tests/own/slice` and `smoke` whole and a
    fixed eighth of the other suites, by name hash (`--all`: every test), plus the two deepest
    nesting tests, in four actions, through a daemon on 8 threads under a chaos seed and
    in-process on one (so link, `-c` and `-fsyntax-only` also compare 1 and N threads), byte
    for byte; two directories with one argv, two programs defining one
    class name differently (#185), edits, fallback, and a killed daemon. One daemon (one pid,
    pinned by a first request) must answer until it is killed, so a crash hidden by the
    client's retry fails it. `KNOWN` lists the divergences of the default run that have their
    own issue (none today).
  - `daemon.process` (`tests/daemon/process.py`, #98, #746): a fixture driver parks a pooled build
    after it has read and analysed its sources. Edits restart the build and get the cold bytes
    and diagnostics, with the restart counted in `ctl status`. A disconnected client stops,
    a client whose daemon died retries, and a shutdown leaves a new daemon to answer the next
    request. Concurrent clients include two requests of one file and a distinct build root
    reaching the checkpoint while the first is parked.
  - All run on macOS and Linux.
  - `daemon.compare`, `daemon.process`, `daemon.trace`, `unit.linux_basic_asm` and
    `unit.linux_elf_comdat` put their private daemon runtimes under `TMPDIR` when the socket path
    fits, with `/tmp` as the fallback for long paths (#793, #1273).
- The first persistence slice (#17) shares phase-1 and phase-2 source preparation in immutable
  mapped snapshot files. A selected query cell can be loaded at quiescence with its direct input
  dependency fingerprints and then verified by the usual engine path. The lexer borrows mapped
  normalized text bytes. The storage switches, scope and benchmark are in
  [Engine storage](docs/engine-storage.md). Cell and slot arena blocks can use process-private
  files with heap fallback.
- Not yet:
  - `progress` messages and `build_time` (#140);
  - the persistent cache handoff and the file table in it (#17), and the β test that needs it:
    the cache across a restart.

## The LSP front and the editor view (#99, 2026-11-18)

`ycxx lsp [--root DIR]` is an editor's language server, per daemon design §6. It reaches the
daemon of its scope (starting one) and sends `lsp`; the server runs in the daemon. From then on the
editor's bytes go to the daemon as they are, one thread per direction, and the daemon's come back
in `lsp` frames (`LspOut`), the last of which carries LSP's exit status (1 after `exit` without
`shutdown`), which `ycxx lsp` exits with.

- The editor view (§6.1–§6.3):
  - `pp::Buffer(PathId) -> optional<text>`, an input at `editor` durability, provided as nothing
    (`pp::no_buffer`) until the daemon sets it;
  - `pp::View` (`disk`, `editor`) in `pp::Config`, part of its fingerprint, so an editor-view
    region or session is never a disk-view one's;
  - in the editor view `EngineHost` reads an open buffer first, then `FileContent`, and the unit's
    own text is read that way too, never from `FileText`. Only the editor view fetches `Buffer`,
    so a keystroke leaves every disk-view cell green by the durability shortcut.
  - `compile::run` takes `Units{view, first}`: the editor view's units are numbered from 2^31,
    one per document. `compile::sources_of` and `compile::check_one` (one unit of a served
    request, `-fsyntax-only` with its preprocessing and language options).
- `ycxx.lsp` (`src/lsp/`): `:json` (a JSON value, parser and printer, and a codec for aggregates
  by reflection), and the session: `Content-Length` framing, `initialize`, `shutdown`, `exit`,
  full-text `didOpen`/`didChange`, `didSave`, `didClose`, and `publishDiagnostics` for every open
  document after each batch of changes (what arrived together is diagnosed once, in one call),
  to the URI the editor opened, with UTF-16 columns. Requests before `initialize` get -32002,
  after `shutdown` -32600. `lsp::parse_messages` reads the compiler's `path:line:col: error:` lines.
- The daemon: a document is checked with the last served `compile` request that compiled it,
  found by real path (else alone in its directory), in the editor view, under the engine lock.
  One freshness batch and one setting of the buffers serve a batch of documents; each buffer is
  set under the editor's name and its real path, and unchanged ones keep their value. Stopping
  the daemon ends its LSP sessions. `ctl status` counts `diagnoses`.
- Tests: `unit.lsp` (JSON, URIs, messages, two scripted sessions, framing); `unit.daemon`'s
  `check_requests`; `unit.pipeline_incremental`'s `editor_view` (an unsaved header and the unit's
  own buffer reach the editor view, a closed buffer falls back to the disk, and a build after a
  keystroke executes and validates nothing); `daemon.lsp` (`tests/daemon/lsp.py`): editing (open,
  an unsaved error at its position, a build after it equal to a cold compile of the disk with
  nothing executed or validated, an unsaved header breaking its includer and closed again, a
  save), a unit and header opened through a symbolic link, a build sent during a `didChange`
  (the saved file's object, the buffer's diagnostics), 600-error answers crossing large edits,
  the exit status, and `ctl shutdown` with a session open.
- A document's command line (#167, §6.4): the last served `compile` request that compiled it,
  else its entry in `compile_commands.json` at the scope's root (`arguments`, or `command` split
  as a shell's words; its preprocessing options and template depth kept by
  `compile::check_command`, another compiler's other options dropped; the file read again when
  its identity changes; `@file` response files expanded relative to the entry's directory; a
  header or C entry is no unit), else the file alone in its directory. `daemon.lsp`'s
  `compile_commands` covers both forms, a response file, a header and a C file, a directory
  spelled through a symbolic link, and a change (mutation-checked). The check also keeps
  `-nostdinc` and `-nostdinc++`. Options `pp` does not take yet (`-include`, `--sysroot`, …) are dropped.
  [#251](https://github.com/yrashk/ycxx/issues/251) · [#997](https://github.com/yrashk/ycxx/issues/997)
- Not yet: stopping a diagnosis on a newer edit,
  `$/cancelRequest`, debouncing and interactive priority (#168); `buffers_in`, the disk-view
  assertion and one view per session (#169); go-to-definition from a declaration part (a
  parameter's type, a base) and of locals (#242); open documents as eviction roots (#17).
- Go-to-definition and headers' diagnostics (#170): `textDocument/definition` asks the daemon
  (`lsp::Define`), which checks the document's unit in the editor view and runs
  `pipeline::DefinitionAt{unit, file, offset}`: the offset's token in the region (its site, a
  macro's at its name), the innermost item, the node `ast::Red::find` reaches in that item's
  function body, and the entity a `DeclRef`, `Member` or `Call` there names; then that entity's
  first declaration (`FrontResult::declared`, kept with errors too) mapped back to a file and an
  offset. Diagnostics located in a file the editor has not opened are published under it and
  cleared when no document reports them; a header's message is rendered against its open buffer
  in the editor view. `daemon.lsp`'s `definitions` covers a header's function, unsaved text, a
  header's warning shown, moved by an unsaved edit, and cleared (mutation-checked).

## The L1 interpreter (#25, 2026-11-18)

`src/interp/` (module `ycxx.interp`), from the [optimization design](docs/design/optimization.md)
§2.3 and §10; unit test `unit.interp`, on bodies written in L1's text form.

- `evaluate(host, interners, target, entry, name, args, config)` is a `query_task`: the step
  machine runs until it needs a callee's body (`host.body(e)`, in the engine `function_ir`) or a
  global (`host.global(e)`, `constant_value`), which it fetches lazily with `co_await`, once per
  evaluation; it polls `host.stop_requested()` every `poll_interval` steps. Calls push frames on
  the machine's own stack. Limits: a step budget counted in nodes and a call depth ([implimits]).
- Modes: `constant` ([expr.const] holds; `is_const_eval` is true and `consteval_br` takes its
  consteval edge) and `runtime` (non-constexpr calls, globals read and written, the erroneous
  value is zero).
- Abstract memory: objects are a type and one cell per scalar leaf, each with its lifetime
  (before, alive, after), whether it holds an indeterminate, erroneous or real value, and the
  value. Pointers are `(object, access path)` plus "one past a non-array object"; they never
  become integers.
- Checks, each a `Failure` with a `Reason`, an argument and the stack of `(function, node)` sites
  (innermost first): signed and `nuw` overflow, division by zero and unrepresentable quotients,
  shift counts, indeterminate and erroneous reads, accesses outside the lifetime or after the
  storage ended, through null or past the end, through a different type, stores to const objects
  and string literals, unsequenced conflicts in `useq` groups, pointer arithmetic out of bounds or
  on the wrong type, differences across arrays, unspecified pointer comparisons, `void*` casts to
  the wrong type; in constant mode also reads of globals not usable in constant expressions,
  writes to objects from outside the evaluation, volatile accesses, non-constexpr and undefined
  callees, `reinterpret_cast`-like ops and `asm`.
- The dispatch table is `ir::eval_table<Ops, …>()` with `complete`: every op has a handler. Ops
  outside the supported subset (`copy`, unions, dynamic storage, virtual calls, atomics,
  EH, coroutines, reflection) fail with `Reason::unsupported`, naming the op. (Floating point
  is evaluated since #336.)
- Remaining in #25: whole-program runs with host I/O, general record storage, and the
  under-construction state of [class.cdtor].

## Constant evaluation in the front end (ε slice, #87, 2026-11-18)

`sema::Evaluator` (`src/sema/eval.cppm`) replaces the α folder: an analysed expression becomes the
L1 thunk `{ return e; }` through `function_ir`, and the interpreter runs it in constant mode inside
the cell that analyses the expression (query-engine.md §B7). `parse::Program` answers its
questions: `function_ir` of a function defined in an earlier item (memoized per item), and a
namespace-scope variable's type and, if usable in constant expressions, its value.

- `constexpr` functions and variables (namespace and block scope; a constexpr variable is const
  and must be constant-initialized), `consteval` functions with immediate invocations replaced by
  their value outside an immediate function context, `constinit`, and the redeclaration rules of
  [dcl.constexpr]/1.
- Native collection omits ordinary named namespace-scope immediate functions whose bodies
  have neither static locals nor closures. Their declarations and analysed bodies remain
  available for constant evaluation. [#1006](https://github.com/yrashk/ycxx/issues/1006)
- Nonvirtual `consteval` member functions and function template specializations with neither
  closures nor static locals can link without producing a native definition. The interpreter
  still reads their checked bodies. The object pipeline emits a definition when lowered code,
  tables or data refer to it, including references from retained immediate bodies.
  [#1097](https://github.com/yrashk/ycxx/issues/1097)
- An `if consteval` constant-evaluated arm can directly call a private static constexpr
  helper defined inside an ordinary non-template class. Native emission can omit the helper
  when the object needs neither a direct call nor its address. An emitted definition keeps
  its weak inline binding. Both arms are checked and the interpreter can call the helper.
  [Design](docs/design/native-private-constexpr.md) · [#1174](https://github.com/yrashk/ycxx/issues/1174)
- `consteval` constructors, in class and out of class, among the other constructor specifiers.
  `Sema::construct` folds a call of one outside an immediate function context, including an
  implicit conversion of an argument or operand, into its value (`Sema::immediate_invocation`,
  shared with consteval function calls). A selected operator whose operand conversion is not
  constant is an error rather than a fall-back to the built-in operator. Every declaration must
  agree on `consteval`, and a destructor cannot be `consteval`. Native collection omits consteval
  constructors, of class templates and constructor templates too.
  [#102](https://github.com/yrashk/ycxx/issues/102)
- The evaluator lays out non-polymorphic classes with user-declared constructors, no bases and no
  destructor, whatever their fields' access, as it does flat aggregates, so constructor calls
  (`constexpr` and `consteval`) evaluate, and `T x(args)` at namespace scope is constant-initialized
  when its constructor call is constant. Class-typed fields and bases are not laid out yet
  ([#25](https://github.com/yrashk/ycxx/issues/25)). [#102](https://github.com/yrashk/ycxx/issues/102)
- `static_assert(e)` and `static_assert(e, "message")` at namespace and block scope (the
  `StaticAssert` node); `if consteval` and `if !consteval` (`IfConsteval`, lowered to
  `consteval_br`). Class member assertions check the class prefix and available bases.
  A class template defers dependent assertions until required completion. A failed assertion
  propagates through required field and base completion without exposing a valid layout.
  [#257](https://github.com/yrashk/ycxx/issues/257)
- Typed addresses of elements and rows of named namespace-scope constexpr arrays
  ([conv.array], [expr.const], #111). The interpreter initializes scalar cells from the nested
  aggregate value on first use. Stored pointers retain the subarray path and element index,
  including one-past indices, for subsequent arithmetic and dereference ([expr.add]). An erased
  one-past pointer fails conversion back to a typed pointer in constant evaluation ([expr.const]).
- Constant array bounds from const integral locals (#58), and address constants of variables and
  functions (#57) as `VLvalue{VEntity, path, byte offset}`. The pipeline emits data relocations
  with the entity's symbol and byte addend on ELF and Mach-O, including references to symbols
  defined in another translation unit (#101).
- Scalar enumeration values work in constexpr variables, calls, casts and switch conditions.
  The interpreter preserves the enumeration's nominal type in L1 and memory. The underlying
  type supplies its integer representation. Constant evaluation diagnoses an evaluated cast
  outside a non-fixed enumeration's range. Class-valued allocation remains a separate slice.
  [Design](docs/design/constexpr-enum-values.md) · [#262](https://github.com/yrashk/ycxx/issues/262)
- A static constexpr data member of flat aggregate class type can be initialized from a typed
  braced expression such as `static constexpr Pt q = Pt{7, 8};`. The direct-list spelling
  `static constexpr Pt q{7, 8};` also works in a non-template namespace-scope class without
  bases. The aggregate has trivial destruction and public non-volatile integral fields up to
  64 bits. Constant evaluation records the field values and code generation emits addressable
  storage for runtime use ([class.static.data], [dcl.init.list], [dcl.init.aggr]).
  [#262](https://github.com/yrashk/ycxx/issues/262)
- Both native targets give an externally linked in-class static constexpr data member of an
  ordinary namespace-scope class one address across translation units when its stored value needs
  no relocations or runtime work. A member whose owner or declared type names an unnamed namespace
  has local symbol binding.
  [#903](https://github.com/yrashk/ycxx/issues/903)
- In-class `static constexpr` arithmetic members of namespace-scope class templates use the same
  coalescing rules for initialized storage. Separate objects observe one address for each externally
  linked specialization. An unnamed-namespace template argument keeps the member local.
  [#1038](https://github.com/yrashk/ycxx/issues/1038)
- Inline static integral, pointer, and supported floating-point members in ordinary
  namespace-scope classes without bases use constant direct-list and copy-list initialization.
  This also covers one-dimensional arrays of those types with a written bound. Omitted array
  elements are value-initialized and the members have addressable static storage
  ([class.static.data], [dcl.init.list], [dcl.init.aggr]).
  [#964](https://github.com/yrashk/ycxx/issues/964) ·
  [Inherited initializer lookup #967](https://github.com/yrashk/ycxx/issues/967)
- Constant expressions can read fields of an already initialized namespace-scope constexpr
  aggregate with a trivial destructor, no bases, and ordinary public integral and enumeration
  fields whose scalar representations fit in 64 bits. The interpreter uses the ordinary record
  layout and field indices, including for reads through a pointer during evaluation. It preserves
  nominal enumeration types for comparisons and template arguments and rejects reads of mutable
  fields of the stored object ([expr.ref], [expr.const], [dcl.init.aggr]).
  [#862](https://github.com/yrashk/ycxx/issues/862) · [#876](https://github.com/yrashk/ycxx/issues/876)
- A constant expression can read a scalar field through a reference to an indexed element of
  a namespace-scope constexpr array of supported flat aggregates. L1 keeps the array element
  and field in the address path and checks the index against the array bound ([expr.sub],
  [expr.ref], [expr.const]). [#1403](https://github.com/yrashk/ycxx/issues/1403)
- Constant evaluation supports named one-dimensional automatic arrays of nonempty nonunion
  aggregates with trivial destruction and direct, non-overlapping integral or enumeration
  fields. A braced initializer initializes those fields directly, including zero initialization
  of omitted scalar fields when no default member initializer applies. Each field becomes
  readable at its own initialization step. Element aliases retain the array's bounds and storage
  lifetime, including through a range-for's reference. Direct initialization from a conditional
  prvalue with a value-initialized arm, such as `b ? Entry() : Entry{9}`, remains open, as do
  whole-element copies, member calls on elements, and separate element lifetime operations
  ([dcl.init.aggr], [basic.life], [expr.const]).
  [#1642](https://github.com/yrashk/ycxx/issues/1642) · [#1140](https://github.com/yrashk/ycxx/issues/1140)
- Constant evaluation of a flat scalar aggregate supplies its initialized field values to
  the selected default member initializer. L1 uses them for direct field reads of the
  initializer's object, including through a conditional glvalue. Reading a later field on
  the executed path fails its lifetime check ([dcl.init.aggr], [expr.const]).
  [#895](https://github.com/yrashk/ycxx/issues/895)
- A constant expression can read public integral and enumeration fields of an ordinary
  flat aggregate temporary with no bases and implicit trivial destruction. Each eligible field
  has its own typed cell. Evaluation rejects a read before initialization
  ([class.temporary], [dcl.init.aggr], [basic.life], [expr.const.core]).
  [#900](https://github.com/yrashk/ycxx/issues/900)
- Constant evaluation can copy a named empty or flat non-union aggregate without bases and
  with implicit trivial destruction into the caller's result storage. A constexpr member can
  use the live result, including a checked pointer into an initialized fixed-size `char` member
  array. L1 diagnoses access through an expired local pointer and an ordinary constexpr member
  call on an expired receiver ([stmt.return], [basic.life], [expr.const]).
  [#975](https://github.com/yrashk/ycxx/issues/975)
- Constant evaluation can read namespace-scope `constexpr` pointers erased to `void`
  ([expr.const], [conv.ptr], #102). The interpreter reconstructs the original scalar or array
  subobject path, including one-past addresses, from the constant's path and byte offset.
- Errors say why: "not a constant expression: <the interpreter's reason>", located at the
  evaluated expression.
- Ordinary nonvirtual immediate member calls with void or integral and enum results up to
  64 bits validate the complete invocation. The evaluator can import supported flat local
  records and preserve the identity of supported unread runtime receivers. The static
  through-object path evaluates the discarded receiver as part of the invocation.
  Reference and field-address results remain tracked in #1183. Static arrow
  indirection checks remain in #1189.
  [#1175](https://github.com/yrashk/ycxx/issues/1175)
- A constant evaluation can bypass a block-scope static aggregate ([expr.const], [stmt.dcl]).
  L1 forms static addresses at their uses and checks the executed declarations for usability in
  constant expressions. The runtime guard still initializes a static local on its first passage.
  [#872](https://github.com/yrashk/ycxx/issues/872)
- Tests: `tests/own/consteval` (spec-only; `constructors/` checked against Clang and GCC); the draft's [dcl.constexpr]/5 ex. 3, [dcl.constinit]/3
  ex. 1 and [stmt.if]/4 ex. 3 pass.
- Not yet (#102): naming a consteval function outside a call, static_assert
  messages that are constant expressions, notes along the call chain, block-scope static constexpr
  variables.

## Classes, δ step 1: structs and aggregates (#86, 2026-11-18)

- **Types and values:** `TClass{entity}` in `types` (complete or not, one type); `VAggregate`
  (an array's or class's element values) in `values`; `DeclKind::data_member`.
- **Front end:** a class definition at namespace scope is a `class_def` item whose members are
  their own items (ast.md §4.2); `Program::class_def(item)` reads them into a `ClassDef` and into
  sema's class table (the unit's `Program::classes` since #115, the stand-in for `class_def`'s
  member list, which the layout reads). A class is complete at an item after its
  definition that it does not enclose (`Questions::is_complete`). Class names as type
  specifiers, elaborated type specifiers and forward declarations, member access (`Member`,
  [expr.ref]), list-initialization of aggregates and scalars (`InitList`, `ValueInit`,
  `DefaultInit`: brace elision, designators, narrowing, [dcl.init.list]/3.2), arrays whose bound
  comes from their initializer, class copies as `lvalue_to_rvalue` of a class glvalue, `x = {…}`.
  Namespace-scope aggregates with constant elements are folded to a `VAggregate`.
- **IR:** members through `ptr_member` field steps; aggregates initialized element by element,
  in an object allocated `zero` when some element is value-initialized; trivial copies and copy
  assignment as `copy`, which `lower` turns into chunked loads and stores. Class layouts (Itanium
  §2.4) are computed by `QueryLayout` from the unit's shapes (`UnitShapes`, #115); data with
  `VAggregate` initializers; Itanium names for global-namespace class types in parameters.
- Tests: `tests/own/classes/s1-*` (spec-only, plus seven from the review). Further cases are
  tracked under classes by value (#116) and records in the interpreter (#25); local classes came with #117.
- Not yet: unions, bit-fields, constructors, bases (access checking came with #119).

- Ordinary named, non-template records with only scalar fields and no bases or unions can use
  nonconstant default member initializers when an automatic declaration uses implicit default
  construction or aggregate initialization. Sema reads each omitted field's retained tokens
  in its complete-class context. IR binds `this` to the destination while evaluating that field
  in declaration order. An explicit aggregate clause keeps its enclosing context
  ([class.mem.general], [expr.prim.this], [dcl.init.aggr], #118).
- Required constant evaluation checks the scalar initializers and retains their values when
  evaluation succeeds. Constant evaluation of destination member reads remains in #25.
  Ordinary non-inline namespace definitions reuse this replay with zero-initialized storage
  and startup initialization in definition order. Block statics, inline and dependent forms,
  other member types and exceptional cleanup remain in #118.
  The fixtures in `tests/own/classes/scalar-default-member-init/` cover both targets in syntax
  checks and native Linux x86-64 execution.
  Diagnostics during replay use the written initializer's source position.
  [#806](https://github.com/yrashk/ycxx/issues/806)
- Default member initializer replay in written constructors uses the initializer's original
  namespace for ordinary and qualified lookup, and its original lexical point for ADL.
  Lookup still sees the enclosing class complete, including its hidden friends.
  [#764](https://github.com/yrashk/ycxx/issues/764)
- A written constructor replays the initializer before taking a stored scalar value, so later
  class members can hide namespace declarations in defaults that evaluated during the member scan.
  [#763](https://github.com/yrashk/ycxx/issues/763)
- Class completion checks non-dependent default member initializer expressions at their original
  lexical point. An unbound class template pattern leaves dependent expressions for a selecting
  initialization ([class.mem.general], [temp.res], [temp.inst]).
  [#732](https://github.com/yrashk/ycxx/issues/732)
- An array member whose innermost element is a scalar (not a pointer to member) keeps no value at
  declaration. Where an initialization selects its default member initializer,
  `AtItem::replay_default_member` replays the tokens for the class or specialization and folds
  them with `Evaluator::evaluate_initializer` into an aggregate literal. This applies to dependent
  bounds and element types (`T w[N] = {}`), partial lists and nested arrays, and generalizes
  #1192's braced `char` arrays, whose mode independence the next item now checks
  ([class.mem.general], [dcl.init.aggr], [temp.inst]/3). A nonconstant array default remains
  #118, and constant evaluation of the object remains #1140
  (`tests/own/templates/array-default-member/`).
  [#1418](https://github.com/yrashk/ycxx/issues/1418)
- The interpreter reports when an `if consteval` took its consteval edge
  (`Outcome::observed_constant_context`, from the `observes_context` flag on its `consteval_br`).
  A default member initializer's value that observed it is not kept at declaration. The use-site
  fold keeps it only in a constant context, because a runtime initialization is not manifestly
  constant-evaluated ([expr.const], [stmt.if]). Those runtime uses are diagnosed until they are
  evaluated at run time.
  [#1459](https://github.com/yrashk/ycxx/issues/1459)
- In non-template code, ordinary scalar aggregates replay written defaults in braced expressions,
  `T()` value initialization, result objects, and aggregate arguments. The expanded runtime path
  checks built-in scalar operations and ordinary calls for nonthrowing behavior. IR binds receiver
  reads to the initialized object. Required constant aggregate initialization evaluates eligible
  scalar defaults in the completed class.
  [#818](https://github.com/yrashk/ycxx/issues/818)
- Function-template bodies replay defaults of ordinary non-template scalar aggregates, for named
  objects and aggregate expressions. Newly replayed runtime defaults use the same nonthrowing
  operation check. [#846](https://github.com/yrashk/ycxx/issues/846)
- A selected braced default member initializer with literal character elements for a fixed
  one-dimensional `char` array supplies the complete array value, including omitted elements.
  Ordinary classes and class-template specializations can use it during implicit default and
  aggregate initialization ([class.mem.general], [dcl.init.aggr], [temp.inst]).
  [#1192](https://github.com/yrashk/ycxx/issues/1192)

- An automatic declaration such as `Point p(3, 4);` can initialize a flat aggregate with
  public arithmetic fields and implicit trivial destruction. The fallback applies when
  constructor resolution finds no viable candidate ([dcl.init.general]). Each expression
  copy-initializes the corresponding field in declaration order, allowing narrowing. An
  omitted field uses its supported scalar default member initializer or is value-initialized.
  Constant initialization uses the scalar element evaluator, with destination-dependent
  constant evaluation tracked in [#25](https://github.com/yrashk/ycxx/issues/25).
- An automatic runtime declaration can also initialize an aggregate with one public nonvirtual
  direct base and at least one public arithmetic field. The base has an explicit initializer.
  The first expression is a nonvolatile lvalue of the exact base type, which is a flat aggregate
  with nonvolatile arithmetic fields and trivial copy and destruction. The base copy uses the
  existing subobject layout and is sequenced before the remaining initializers.
  An unqualified `B{…}` prvalue of the exact base type can also supply that element when each
  base field has a separately written positional initializer and the base has a usable trivial
  move constructor. The complete temporary is initialized once before its value is copied into
  the base subobject.
  Omitted trailing arithmetic fields are value-initialized when they have no written default
  member initializer. With a sole base expression, the base must have no conversion functions.
  For a named non-template class at namespace scope, an omitted field's written default can
  use call-free arithmetic on literals and fields of the initialized base. The replay binds
  implicit and explicit `this` to the destination object. Explicit clauses keep the enclosing
  receiver and an explicit field initializer takes precedence over the default.
- Parenthesized arrays, nested fields, unions, reference members, bit-fields, and nontrivial
  destruction remain in [#301](https://github.com/yrashk/ycxx/issues/301). The issue also tracks
  multiple bases, other base prvalues, default member initializers with calls, mutation or reads
  outside the base, constant evaluation of base copies, static storage and functional casts.
  Linux x86-64 runtime conversions between floating-point and integer types remain in
  [#201](https://github.com/yrashk/ycxx/issues/201).
  Spec-only fixtures are in `tests/own/parenthesized-aggregate/`.

- Ordinary mutable data members can be modified through a const object. A member access
  preserves volatile and the field's declared qualification. Native const globals with mutable
  subobjects are placed in writable storage, including through records and arrays (#373).

## Classes, δ step 2: member functions and `this` (#86, 2026-11-18)

- Member functions (in-class and out-of-class definitions, `const`/`volatile`, static) and static
  data members (in-class `static const`/`constexpr` values, out-of-class definitions) are entities
  scoped to their class (`EntityRec::scope`); `class_def` lists them in `ClassRec::members`.
  A qualified declarator `C::m` defines the member C declares with that type.
- In-class conversion-function declarations ([class.conv.fct], #339) can use a class alias
  as the target, including a dependent alias in a class template. The canonical target type
  and the member function's qualifiers distinguish declarations. A named `static_cast`
  can select a directly declared or instantiated conversion function with an exact non-class
  value result, checking the implicit object's cv qualifiers and value category. Ordinary
  in-class conversion functions may declare `&` and `&&` qualifiers. Among viable exact-result
  candidates with the same cv qualifiers, an rvalue prefers the `&&` candidate.
  Ordinary in-class conversion functions accept bare `explicit` in either order with
  `constexpr`. The exact-result named `static_cast` can call them and copy-initialization
  excludes them (#1468). General conversion selection remains tracked in #339.
- Bodies of member functions are complete-class contexts: class member lookup comes before
  namespace lookup, and their class is complete there. `This` and implicit member access
  (`Member` on `this`), bound member functions (a `Member` of type `bound_member` that only a call
  may follow), `MemberCall` with the object's address as its first operand, `C::m`, and the const
  rule of the implicit object parameter ([class.this]). Reads of a static data member usable in
  constant expressions become its value (no odr-use).
- IR: `this` is the entry block's first value parameter (`FunctionInput::this_type`), as the
  Itanium ABI passes it. Linkage names are `<nested-name>`s with the member's cv-qualifiers,
  checked against Clang's symbols.
- In-class nonvirtual definitions of ordinary functions in namespace-scope class templates can
  emit weak definitions on ELF and Mach-O. This covers static and nonstatic members with bodies
  eligible for the existing inline coalescer. Specializations naming unnamed-namespace types
  retain local binding, including a value argument of such an enum type. Explicit non-inline
  specializations retain strong definitions. [#1051](https://github.com/yrashk/ycxx/issues/1051)
- Tests: `tests/own/classes/s2-*` (spec-only, plus three from the review): 12 of 13 pass; the constexpr one waits for records
  in the interpreter (#25). In-class member functions are weak definitions on both targets (#422, #124).
- First-declaration deleted copy assignments can carry a ref-qualifier. Defaulted assignment
  remains open. Other ref-qualified members are under #311 below.

## Ref-qualified member functions (#311)

- Ordinary non-template members in namespace-scope classes distinguish lvalue and rvalue
  receivers, including cv-qualified overloads and out-of-class definitions. Nested Itanium
  names encode the ref qualifier for mixed-object calls.
- Members of namespace-scope class templates, their nested classes, partial and explicit
  specializations and member templates keep their ref-qualifier through instantiation, and
  calls rank the implicit object parameter by the object's value category. Out-of-class
  definitions of members and member templates match on the ref-qualifier, and a redeclaration
  that corresponds only after instantiation is diagnosed. A member function template's Itanium
  `<nested-name>` encodes the ref-qualifier, checked against Clang's symbols in both link
  directions.
- GCC mode also permits an unqualified nonstatic member or conversion to overload its
  ref-qualified counterpart. Spec and Clang modes diagnose the corresponding declarations.
- Ref-qualified virtual calls and joint ranking of ref-qualified member and nonmember operators
  remain open here. Ref-qualified members of local classes wait for local classes
  ([#117](https://github.com/yrashk/ycxx/issues/117)); out-of-class definitions of
  nested-class members of class templates wait for [#1187](https://github.com/yrashk/ycxx/issues/1187).

## Classes, δ step 3: constructors, destructors, references, overloading (#86, 2026-11-18)

- **References** ([dcl.ref], [dcl.init.ref]): `T&` and `T&&` declarators, reference locals,
  parameters, results and members; direct binding, and binding a const reference to a
  materialized temporary (`Materialize`), extended for a local reference ([class.temporary]/6).
  In the IR a reference is the pointer to its object (`TypeFacts` maps reference types to
  pointers); a reference local or parameter is that pointer, not an object. Itanium `R`/`O`.
- **Overloading** ([over.match]): namespace-scope and member function overloads, and
  constructors; implicit conversion sequences ranked as exact match (with qualification and
  reference-binding tie-breaks), promotion, conversion; ambiguity and deleted functions
  (`= delete`) diagnosed.
- **Constructors and destructors**: declarations in the class, mem-initializers and default
  member initializers (analysed again in each constructor, a complete-class context),
  `CtorCall` for every initialization form (`T x(a)`, `T x{a}`, `T x = a`,
  `T(a)`, `T{a}`, value- and default-initialization, copies), `MemberInit` statements in
  declaration order, `Destructor`/`DestroyMember` for members after the body. Implicit copy
  constructors and destructors that are not trivial are synthesized (`Program::synthesized`).
  A class with user-declared constructors is not an aggregate.
  Ordinary constructor declarations and definitions accept `constexpr`, and the specifier is
  checked across an out-of-class definition ([dcl.constexpr], #157). These constructors use the
  ordinary runtime construction path. Constant evaluation of class objects is tracked in #25,
  and named exception specifications are tracked in #219.
- Constructors and constructor templates, in ordinary classes and class templates, can spell
  `explicit` or `explicit(constant-expression)` in class, in either order with `constexpr`.
  A matching out-of-class definition inherits the property. Copy-initialization excludes these
  constructors. Direct initialization includes them. Copy-list-initialization ranks them and
  diagnoses an explicit winner, including a default constructor or a temporary for a reference
  argument ([dcl.fct.spec], [over.match.ctor], [over.match.copy], [over.match.list], #157, #715).
  The condition is a contextually converted constant expression of type `bool`, and narrowing
  is diagnosed. A condition that depends on a class template's parameters is evaluated when the
  member is instantiated. One that is value-dependent on a constructor template's own parameters
  is kept as an atom and read again after deduction, once the template's constraints are
  satisfied. A substitution failure there fails deduction, and an explicit specialization leaves
  copy-initialization ([temp.deduct.general], [over.match.funcs.general]). Conversion functions
  (#339) do not take the specifier yet; deduction-guides do (#316).
- Constructor templates take part in overload resolution only when their associated
  constraints are satisfied ([temp.deduct.general]/5, #1398). An atom spelled in a member of a
  class template specialization, such as a member template's constraint, reads the enclosing
  class's parameters with the specialization's arguments.
- Both native targets coalesce eligible inline complete-object constructors across translation
  units. This covers in-class bodies and out-of-class bodies marked inline by a declaration
  ([dcl.inline], [basic.def.odr]). Translation-unit-local classes retain local constructor
  symbols, and non-inline out-of-class definitions retain strong symbols.
  [#1096](https://github.com/yrashk/ycxx/issues/1096)
- A default, copy or move constructor, a copy or move assignment operator and a nonvirtual
  destructor can be defaulted on their first declaration in a class or class template
  ([dcl.fct.def.default], #157). The member stays declared: it takes part in overload
  resolution, access checks and aggregate rules, and is not user-provided.
  - When its class is complete, or its specialization is instantiated,
    `Program::classify_special_member` runs overload resolution for each base and member from
    the class's context and finds the function deleted, trivial, nontrivial, or beyond ycxx
    (diagnosed at use). A type that the implicit declaration cannot have deletes it, and a deleted
    move is no candidate ([over.match.funcs.general]). A class whose implicit copy depends on a
    defaulted one is classified the same way.
  - A trivial operation is a representation copy, `DefaultInit` or `ValueInit`. A nontrivial one
    is defined where something uses it: initialization or assignment of the bases, then the
    members, with an array element by element. The definition is `constexpr` and weak, and its
    analysis for a specialization is at the end of the unit.
  - The implicit exception specification is decided where `noexcept` asks for it
    (`Program::defaulted_nonthrowing`); it does not include subobject destructors ([except.spec]).
  - A class is POD for layout by the `-fcompat` mode's definition, where defaulted and deleted
    constructors are not user-provided: C++11's in Clang mode on arm64 macOS, C++03's otherwise
    (public members, no bases).
  - Outside the class (#1541, #1565): a non-template, non-union class with no bases or virtual
    functions, defined in an enclosing namespace, can default its default, copy and move
    constructors and its destructor when its members are scalars or arrays of them (a const one
    with an initializer, for the default constructor), and its copy and move assignment when
    they are scalars, none const, volatile or `[[no_unique_address]]`. Such a definition is
    user-provided and keeps the first declaration's exception specification. An array member
    in the assignment case gets a diagnostic about the signature instead.
  - Not yet: local classes, defaulting outside the class for classes with bases, class members
    or virtual functions, a virtual assignment operator, member pointers in assignment.
  [Design](docs/design/defaulted-special-members.md) · [#157](https://github.com/yrashk/ycxx/issues/157)
- An ordinary namespace-scope class with no bases and a deleted implicit copy can synthesize its
  available implicit move constructor for scalar fields and class fields with proven nonthrowing
  initialization and destruction. The fields initialize in declaration order from the source
  xvalue. The class retains aggregate initialization, and the inline move definition is emitted
  only on use.
  [Design](docs/design/implicit-move-constructors.md) · [#747](https://github.com/yrashk/ycxx/issues/747)
- **Cleanups** (ast.md §3.7.3 as revised by #135): `LocalVar::destructor`; `function_ir` runs
  destructors on every scope exit from its own scope stack (block end, `return`, `break`,
  `continue`) and ends a full-expression's temporaries at its end. Statements carry no cleanup
  lists; the `Cleanup` record is gone.
- Both native targets emit `C2`/`D2` alongside eligible written, out-of-line `C1`/`D1` definitions
  (on Mach-O, a second symbol at the same address).
  An ordinary, externally linked, nonpolymorphic class with no bases and only supported
  scalar fields uses one machine body and ELF range for each pair. Named signature types
  must have known layout and linkage. Callback signatures and class parameters passed by
  value are outside this scope. GCC and Clang peers check the raw entry names, an adjusted
  base pointer, and construction and destruction order.
- Both native targets can define the out-of-line virtual destructor of an ordinary fieldless,
  baseless root when the first public virtual is a peer-defined non-inline `int()` key
  function and the second is the public destructor. Both declarations and the destructor
  definition use bare `noexcept`. The `D1` and `D2` entries share a body that restores
  the peer's root table address point before calling a directly declared nonthrowing C++
  observer with `this`. `D0` calls `D1`, then calls implicitly declared global sized
  `operator delete` with the original pointer and the complete root size.
  [Itanium ABI §2.5.2](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vtable-components) ·
  [#136](https://github.com/yrashk/ycxx/issues/136) · [#659](https://github.com/yrashk/ycxx/issues/659)
- On Linux x86-64, a derived class with an empty out-of-line virtual destructor can own its
  32-byte table and single-inheritance RTTI. It has one public nonvirtual primary base with
  one directly written pointer field with a builtin pointee type and a peer-owned public
  virtual destructor. `D1` and `D2` share a body
  that restores the derived address point and calls the base's `D2` at offset zero. `D0` then
  deallocates the original pointer with the complete 16-byte object size. The derived descriptor
  imports the peer's base descriptor. The destructor specifications can be implicit. A unit
  containing only the declarations leaves ownership with the units defining the keys.
  [Design](docs/design/linux-derived-virtual-destructor.md) · [#999](https://github.com/yrashk/ycxx/issues/999)
- A non-template constructor can delegate to another ordinary constructor of its class with
  a sole parenthesized or braced initializer ([class.base.init], #157). The target resolves in
  the complete class and constructs the same object. The delegating body starts after the
  target and its initializer temporaries finish. The delegator supplies no member or base
  initialization. Direct self-delegation is diagnosed. Implicit trivial or defaulted copy targets,
  virtual layout, exceptional cleanup, and constant evaluation of the object remain in
  #157, #309, and #25.
- Tests: `tests/own/classes/s3-*` (spec-only, plus two from the review): 16 of 16 pass. 10 more draft examples pass.
- Not yet: temporaries with destructors created in a conditional operand, value-initializing a
  member subobject whose class has an implicit or defaulted nontrivial constructor, and other
  `= default` forms (#157, #244). Pack expansions in mem-initializers: "Pack expansions in
  mem-initializers (#1400)" below.

- Ordinary explicit class returns copy-initialize from an eligible named local or
  parameter as an xvalue, including parenthesized names. A written move constructor
  can be selected even when the copy constructor is deleted. [#749](https://github.com/yrashk/ycxx/issues/749)

## Nested classes of ordinary classes (#306)

- A class defined in a class with no template-head and no specialization, at any depth, is a
  member class ([class.nest]): `Program::named_nested_class` selects its item, and
  `class_definition` gives it the entity `{class_, name, scope = enclosing class}`. Its declarators
  after the braces are members of the enclosing class (`member_class_definition`).
- `class-key identifier;` in such a class declares a member class (`member_class_declaration`),
  and `typedef struct I { … } T;` defines I and declares T. A member class conflicts with a
  same-named typedef-name or enumeration, and cannot have its enclosing class's name.
- `class O::I … { … };` at namespace scope defines one ([class.pre]/3): O must be complete and
  have declared I, and the namespace must enclose O (`member_class_qualifier`). That item binds
  no name in its namespace (`declared_in`), and its members' item scope is `O::I`.
- The enclosing class's `class_content` lists each member class once as a nested type, so
  qualified lookup (`O::I`), access checks and `earlier_member` find it. Redeclarations of a member
  class use the same access.
- Unqualified lookup from a nested class's members goes on to each enclosing class
  (`Sema::enclosing_class`). It sees the complete class in a complete-class context. In an
  incomplete enclosing class it searches preceding members, then bases, before continuing to
  outer scopes. Qualified names see the same enclosing declarations. Lookup checks inherited
  access and respects names hidden by non-types. Static functions need no receiver for either
  spelling. Non-static data members can be named without an object in an unevaluated operand.
  [#1216](https://github.com/yrashk/ycxx/issues/1216)
- A nested class is complete in its enclosing class's complete-class contexts and implicit
  special members (`Program::is_complete`, `within_outer_class`). `analyze_group` completes a
  class, then its nested classes, then the members' bodies (`complete_class_items`); `front.cppm`
  emits nested classes' members, and a nested class's constexpr static members are vague linkage.
- Virtual members and the special-member features gated on "a class at namespace scope" accept a
  class nested in ordinary classes (`Program::outermost_class_scope`); peer-profile layouts still
  exclude classes with nested types.
- Tests: `tests/own/classes/nested` (47, including three independent #1216 controls), 5 draft
  examples, and `unit.darwin_nested_classes` / `unit.linux_nested_classes` (two units sharing a
  nested class; on Linux also every run test).
- Not yet: member class templates (#340) and a class head qualified by a namespace
  (`class N::X { … }`, #1188). Found on the way: #1201 (members declared after a body), #1202
  (hiding in the class's own scope), #124 (`static inline` members are strong).

## Local classes (#117)

- A class defined in a block ([class.local]) is an item of its own
  ([design](docs/design/local-classes.md), decided by Fable): the splitter cuts the class-specifier
  out of the function body, consteval block or declaration (a lambda in an initializer) that holds
  it (`Splitter::local_classes`, `Item::local_owner`), and its members are member items. The name
  index puts it on a level of its own (`ns::(local k)`), so no lookup outside its block finds it.
- The body's analysis defines it where the class-specifier is (`Parser::define_class`,
  `Program::define_local_class`): an entity local to the function or lambda
  (`EntityRec::local_class`), numbered among its local classes of that name in token order; its
  name in the block; and the class itself through `ItemAnalysis` and `class_def`, so constructors,
  destructors, default member initializers, special members, emission and constant evaluation are
  those of any class. Unnamed local classes (named by a typedef for linkage) and declarators
  after the `}` work, as do `struct X;` and `struct X* p;` naming no class, which declare a
  local class a later definition completes ([basic.scope.pdecl]). Local items' ids include their
  owner's, so same-shaped classes of different functions stay distinct.
- Members see the block as of the class-head (`sema::OuterBlock`): its types, using-declarations
  and using-directives, namespace aliases, `using enum`s, static locals, and constants for their
  values. An automatic variable is usable
  only unevaluated ([basic.def.odr]). Lookup reaches the block after the local class and before
  the function's class ([basic.lookup.unqual]), including a class still being defined around a
  lambda in a static member's initializer. Access is the function's ([class.local]/1).
- In a template's definition a local class is dependent; each specialization of the function,
  including a member function template of a class template, has its own, instantiated from the
  item with the arguments bound (`replay_class`). Its members are emitted where used.
- Member templates, static data members and friend function definitions in a local class are
  diagnosed ([temp.mem], [class.static.data], [class.friend]).
- Symbols are Itanium `<local-name>`s with discriminators (`_ZZ1hvENK1A3getE_0v`), checked
  against Clang 23 objects.
- `#include <iostream>` hello: 2,698 errors before, 2,660 after. libycxx's `resize_and_overwrite`
  guards and `tuple_cat` plan compile; its ctype and errno tables wait on constant evaluation of
  class and array subobjects (#1140).
- Tests: `tests/own/local-classes` (39 spec-only, Clang 23 and GCC 16 agree; 3 for libycxx's
  shapes and 1 from the review), `tests/own/abi/local-class-names.run.cpp` (a Clang peer), `unit.items` (the
  splitter), and the δ tests `s1-locals-scopes` and `s1-elaborated`.
- Not yet: block-scope enumerations ([#1442](https://github.com/yrashk/ycxx/issues/1442)), a
  member class of a local class defined later in the block
  ([#1443](https://github.com/yrashk/ycxx/issues/1443)), local classes with virtual functions
  ([#1155](https://github.com/yrashk/ycxx/issues/1155)), constant evaluation of local
  aggregates with class or array members ([#1140](https://github.com/yrashk/ycxx/issues/1140)),
  which uses of an enclosing constant are odr-uses, shared with lambdas
  ([#1447](https://github.com/yrashk/ycxx/issues/1447)), and the discriminators where Clang and
  GCC disagree ([#1448](https://github.com/yrashk/ycxx/issues/1448)).
  [[class.local]](https://eel.is/c++draft/class.local) · [#117](https://github.com/yrashk/ycxx/issues/117)

## Nested classes of class templates (#1187)

- Named non-template member classes can be declared and defined inside a primary class template
  or its partial or explicit specializations. Each member class belongs to the selected
  specialization. An implicit instantiation declares its member classes and replays a definition
  with the enclosing arguments bound when the member must be complete ([temp.inst]/3).
- Unqualified lookup inside the nested class searches the enclosing classes and their nondependent
  bases before the enclosing template parameter scope ([temp.local]/7).
- Out-of-class definitions remain in [#1187](https://github.com/yrashk/ycxx/issues/1187), member
  class templates in [#340](https://github.com/yrashk/ycxx/issues/340), and virtual members in
  [#1153](https://github.com/yrashk/ycxx/issues/1153).

## Ordinary destructor access (#259)

- Automatic class objects and evaluated expressions of the form `T(args)` or `T{args}`
  check access to the class destructor in the current context. Declaration-only uses
  can still name the type.

## Classes, δ step 4a: enumerations (#86, 2026-11-18)

- `TEnum{entity}` in `types`; sema's enumeration table (`Classes::enum_rec`: scoped or not, the
  underlying type, fixed or chosen to hold the values, the enumerators and their values).
- Unscoped and scoped enumerations (`enum class`, `enum struct`), fixed underlying types,
  enumerator values from constant expressions (narrowing into a fixed type diagnosed), unscoped
  enumerators in the enclosing scope and clashes with other declarations, `E::e`, enumerations
  nested in classes (`C::E`, `C::e`), `using enum` in a block, `enum E` elaborated specifiers.
  Without a fixed type the underlying type is the Itanium compilers' (#220): `unsigned int` or
  `unsigned long` when no enumerator is negative, else `int` or `long`, the narrower that holds
  every value (`tests/own/classes/s4-enum-*`, symbols checked against Clang's). It promotes to
  the type its values need ([conv.prom]/3, `EnumRec::promotion`), and an implicit enumerator
  whose value outgrows its predecessor's type gets a wider one ([dcl.enum]/5).
- Conversions: an unscoped enumeration promotes ([conv.prom]/4) and converts to integers and
  bool; nothing converts implicitly to an enumeration except its own type; a scoped one
  compares only with its own type. A fixed underlying type permits direct-list initialization
  from a nonnarrowing integer, as in `E e{n}`. A one-element copy-list uses ordinary implicit
  conversion to the enum type ([dcl.init.list]/3.8). Overload ranking knows enumeration promotions.
  [#966](https://github.com/yrashk/ycxx/issues/966)
- An enumerator is a `Literal` of its enumeration with an underlying integer constant. L1
  preserves the enumeration type. The interpreter, lowering and layout use the underlying
  integer representation. [#262](https://github.com/yrashk/ycxx/issues/262)
- Linkage names of named enumerations, nested ones included, match Clang's.
- A qualified elaborated specifier such as `enum Box::State` uses type-only lookup for the
  terminal name. A same-name enumerator can hide the enum in ordinary lookup. The elaborated
  specifier requires an existing enum declaration and diagnoses aliases and other tag kinds
  ([basic.lookup.elab], [dcl.type.elab], #761).
- A class member declaration can define a named nested enum and declare ordinary fields in
  the same declaration. The fields participate in lookup, initialization, and class layout.
  Tests in `tests/own/nested-enum-fields/` compare combined and separate declarations and
  cover scoped enum field initializers, and `tests/own/builtins/offsetof-after-nested-enum` their
  offsets. Nested class definitions are in "Nested classes of ordinary classes".
  [#500](https://github.com/yrashk/ycxx/issues/500) · [#306](https://github.com/yrashk/ycxx/issues/306) ·
  [#248](https://github.com/yrashk/ycxx/issues/248)
- Nested scoped enumerators are looked up in their enumeration's scope in ordinary classes and
  class templates. A class member can reuse a scoped enumerator's name, including a declarator
  following the enum definition in the same item. Class-scope lookup still finds unscoped
  enumerators. [#725](https://github.com/yrashk/ycxx/issues/725)
- Nested unscoped enumerators contend with earlier members in ordinary classes and class templates.
  An enumerator can hide a same-scope enum tag in ordinary lookup. The tag remains available as
  a nested-name-specifier or through an unqualified elaborated enum specifier.
  [#731](https://github.com/yrashk/ycxx/issues/731)
- Nested enumeration initializers see the enclosing class prefix by unqualified lookup. An
  earlier enumerator in the same enumeration takes precedence over an enclosing class member
  ([basic.scope.enum], [basic.scope.class]). A preceding non-static data member can be named
  without an object in unevaluated operands such as `sizeof(member)` in an initializer and
  `decltype(member)` in an enum-base. When a call resolves a mixed static and non-static
  overload set from the class prefix, the selected function must be static. Tests cover
  declaration order, ref-qualified candidates, braced arguments, and cold and resident edits.
  [#760](https://github.com/yrashk/ycxx/issues/760) · [#882](https://github.com/yrashk/ycxx/issues/882)
- Tests: `tests/own/classes/s4-*enum*`, `s4-scoped-enum`, `s4-unscoped-enum` (spec-only, plus two
  from the review): 7 of 7.
- Namespace-scope opaque enum declarations ([dcl.enum], #157) supply a complete enum type with a
  fixed underlying type before the enumerator list. Omitted scoped bases default to `int`. Unscoped opaque
  declarations spell their base. Compatible redeclarations and a later definition in the same
  namespace share the nominal type, and the enumerators become visible at the definition.
- A named enum retains its in-list record across synchronous class-template instantiations that
  define other enums. Queries can still use the enclosing enum's fixed underlying type
  ([dcl.enum], [expr.sizeof]). [#897](https://github.com/yrashk/ycxx/issues/897)
- A qualified value expression in an incomplete current class can see earlier static data
  members and injected unscoped enumerators. Within a named enum's own list, qualifying an
  earlier enumerator uses its in-list type. [#857](https://github.com/yrashk/ycxx/issues/857)
- An ordinary class's in-class static data initializer searches its own declarations and
  bases before the enclosing namespace. The base lookup retains ambiguity and access checks,
  including while the derived class is incomplete. [#967](https://github.com/yrashk/ycxx/issues/967)
- Nonempty standalone unnamed unscoped enumeration definitions at namespace scope support fixed
  and inferred underlying types. An `extern "C"` block leaves the enumerators in the enclosing
  namespace. The enum's linkage identity uses its namespace, canonical underlying type and first
  enumerator ([dcl.enum]/12). Signatures that require mangling the unnamed type get a diagnostic.
  Tests cover both targets in `tests/own/namespace-unnamed-enums/` and the draft example in
  `tests/draft/dcl/dcl.enum-2.accept.cpp`.
  [#157](https://github.com/yrashk/ycxx/issues/157) · [Itanium C++ ABI #205](https://github.com/itanium-cxx-abi/cxx-abi/issues/205)
- Nonempty standalone unnamed unscoped enum definitions also work in ordinary classes and class
  template specializations with nondependent enumerator values. Unqualified initializers can read
  earlier class members and earlier enumerators. Their names enter class scope and use the
  ordinary member collision checks. The enum identity uses the declaring class, canonical
  underlying type, and first enumerator.
  [#859](https://github.com/yrashk/ycxx/issues/859)
- A standalone unnamed unscoped member enum can use dependent values. Class completion
  evaluates its list, including earlier enumerators and implicit successors, and checks
  fixed-base ranges or infers the underlying type. The class specialization, first
  enumerator and completed underlying type determine its identity ([temp.inst], [dcl.enum]).
  [#894](https://github.com/yrashk/ycxx/issues/894)
- Named unscoped member enum definitions in class templates defer value-dependent initializers
  and their implicit successors. Specialization replay evaluates each list, including earlier
  enumerator references, fixed-base range checks, and underlying-type inference. Size and
  alignment of an inferred enum with dependent values wait for that replay.
  [#861](https://github.com/yrashk/ycxx/issues/861)
- Named scoped member enums with a supported fixed underlying type publish their declaration
  and layout when the enclosing class is instantiated. Looking up an enumerator instantiates
  the full definition with the owning specialization's template arguments and lexical scope.
  Failed nested demands publish no enumerator values. Class layout and type-only use leave
  the definition and unused default member initializers pending ([temp.inst], [dcl.enum]).
  [#893](https://github.com/yrashk/ycxx/issues/893)
- Further work includes opaque declarations in classes or blocks, unnamed empty definitions,
  unnamed definitions followed by declarators, and `E{}` and `E(v)` expressions
  (#157). Enumerations in a block are tracked in #1442. Qualified lookup of earlier enumerators
  while the unnamed definition is open is tracked in #879.

## Direct explicit destructor calls (#722)

- `object.~Class()` and `pointer->~Class()` support the receiver's own complete class
  with a nonvirtual destructor. A visible type alias for the class works too. The compiler
  checks the destructor's access and deletion before lowering.
- The receiver is evaluated once. The IR calls the selected destructor when it is
  nontrivial and ends the object's lifetime without releasing storage. A trivial destructor
  still ends the lifetime. The native peer test controls the lifetime of each variant member.
- A dependent class name works in instantiated free functions and class template members.
  Unary `noexcept` accounts for the effective exception specification of the selected destructor.

## Classes, δ step 4b: namespaces (#86, 2026-11-18)

- **Scopes:** a namespace is named by its path (`a::b`; `(anonymous)` for an unnamed one);
  `Program` gives each item the namespace whose definitions enclose it, and, for lookup, every
  namespace that encloses that one through inline and unnamed namespaces only, so their members
  are found in the enclosing ones ([namespace.def.general]/7, [namespace.unnamed]). A namespace
  inside an inline one keeps its own inline set (`std::__y1::ranges::__cpo::begin` is
  `std::ranges::begin`), an extension without `inline` of an inline namespace is still inline, and
  making a namespace inline when it is reopened is an error. [#153](https://github.com/yrashk/ycxx/issues/153)
  Namespace entities (`EntityKind::
  namespace_`) are the scope of their members' entities, so `geo::value` and `::value` are two;
  members of an unnamed namespace have internal linkage.
- **Lookup:** unqualified lookup ([basic.lookup.unqual]) walks the enclosing namespaces from the
  point, with namespace-scope using-directives where [namespace.udir]/2 puts their names;
  qualified lookup ([namespace.qual]) in `N::x`, `::x`, `N::C::m`, `N::E::e` and qualified type
  names; block-scope `using namespace`, `using N::x;` and `namespace A = N;`, and namespace-scope
  directives and aliases; directives are transitive ([namespace.udir]/4) and qualified lookup
  follows them ([namespace.qual]/2). Two using-directives that bring in different entities make a name
  ambiguous. Constructors and destructors are not found by name ([class.ctor]/1).
- Block-scope using-declarations make imported enumeration, class and alias names available as
  qualifiers. Lookup considers them alongside local type and namespace aliases, starting with the
  innermost block ([namespace.udecl], [basic.lookup.qual]). [#953](https://github.com/yrashk/ycxx/issues/953)
- **Definitions outside:** `int N::f(…) {…}`, `int N::C::count = 0;`, `N::C::C(…) : … {}` and
  `N::C::~C()`: what follows the declarator-id, the body included, is looked up in that scope
  ([basic.lookup.unqual]/9).
- Linkage names: `<nested-name>`s with namespace prefixes as substitution candidates, and
  `_GLOBAL__N_1` for the unnamed namespace, checked against Clang's symbols.
- Ordinary namespace C++ functions whose adjusted parameter types name an unnamed-namespace type
  use local ELF and Mach-O symbols, preserving their source linkage and mangled names. Native
  references need a definition in the same unit. An unused declaration emits no symbol.
  [Design](docs/design/tu-local-parameter-symbols.md) · [#582](https://github.com/yrashk/ycxx/issues/582)
- The Itanium ABI abbreviates the global `std` namespace as `St` without assigning it a
  numbered substitution (§5.1.5 and §5.1.10). Direct members use unscoped encodings
  and deeper members keep `N…E`. The Linux mixed-object test links calls through the pinned
  libycxx `<cstddef>` byte operator with matching GCC and Clang symbols (#64).
- Tests: `tests/own/classes/s4-namespace*`, `s4-using` (spec-only, plus one from the review): 5
  of 5. Seven more draft examples pass.
- A C or C++ linkage specification with one declaration treats `extern` as present for name
  linkage and for distinguishing declarations from definitions ([dcl.link]). This includes a
  first ordinary namespace `const` variable. A braced linkage block applies the ordinary
  namespace rules. A variable with C language linkage is one in every namespace.
  [#942](https://github.com/yrashk/ycxx/issues/942)
- A qualified namespace function definition inherits C language linkage from a matching prior
  member declaration and uses its target namespace for body lookup. Ordinary and argument-dependent
  calls find the prior declaration and use the same unmangled symbol as C callers (#426).
- An unqualified namespace function redeclaration inherits C language linkage from its corresponding
  earlier declaration in the same namespace. Adjusted parameter types identify the redeclaration
  among overloads ([basic.scope.scope], [basic.link], [dcl.link], #455).
- A second definition of the same ordinary external C-linkage function in a translation unit is
  diagnosed across namespaces. Compatible declarations share the one definition and address
  ([dcl.link], [basic.def.odr], #446).
- Not yet (#153): `using N::f;` at namespace scope, `N::S{…}` as an expression, some clashes
  with namespace names and using-declarations, remaining standard-library linkage
  abbreviations such as `Sa`.

## `<cstdio>`, step 1: typedef-names and alias-declarations (#133, 2026-11-18)

- **Declarations:** `typedef` declarators and `using T = type-id;` at namespace and block scope
  ([dcl.typedef]); `typedef struct S { … } T;`, `typedef struct { … } T;` and the same with
  enumerations, where an unnamed class or enumeration takes the typedef-name as its name
  ([dcl.typedef]/9). A typedef-name may be redeclared in its scope as the same type, and may
  redeclare a class or enumeration of its scope as itself (/5, /6); anything else clashes.
- **Lookup:** a typedef-name is a type name in decl-specifiers, nested-name-specifiers and
  qualified type names (`N::L`); not after a class-key ([dcl.type.elab]/2); not an expression
  ([expr.prim.id]). Block-scope ones hide and are hidden as other block declarations are.
- Entities: `EntityKind::type_alias`, `(scope, name)`, with no symbol. Lookup gives the aliased
  type itself: the `STypedef` sugar of ast.md §5.2 is not built yet (#179).
- Also fixed: a variable declared after an enumeration's braces (`enum E { … } e;`) was not
  found by lookup and had no symbol.
- Tests: `tests/own/typedef` (14, three from the review). Eleven more draft examples pass.
- Not yet (#180): member typedefs and alias-declarations in classes, alias templates (ζ), `typedef`
  of an array of unknown bound, the [dcl.typedef]/9 restrictions on an unnamed class named for
  linkage purposes.

## `<cstdio>`, step 2: using-declarations at namespace scope (#133, 2026-11-18)

- `using N::x, ::y;` at namespace scope ([namespace.udecl]): each declarator introduces what
  qualified lookup finds at its position, as query-engine.md's "Using-declarations" says;
  `Program::using_targets_` keeps them by (item, name ordinal) and unqualified and qualified
  lookup that meet the using-declaration's name take them instead. A later overload of the
  target is not introduced (/11).
- Conflicts ([namespace.udecl]/10, [basic.scope.scope]/4): a target and a declaration of the
  scope, either first, unless they are one entity, both functions, or a type and a variable or
  function. A using-declaration may not name a namespace (/3) or, at namespace scope, a class
  member (/8).
- Tests: `tests/own/using` (10, two from the review). Two more draft examples pass.
- Not yet (#153): enumerators through `using E::e;` (/7), a function target with the same
  parameter types as a function of the scope (/14), `typename` with dependent names (ζ).

## Argument-dependent lookup for unqualified calls (#165, 2026-11-19)

- [basic.lookup.argdep] finds functions in the namespaces associated with class and enumeration
  arguments, including classes' bases at the lookup point and the inline namespace set. Pointer,
  reference, array, function and pointer-to-member types contribute through their component types.
  Ordinary and dependent calls expand class specializations' canonical template type arguments,
  including defaults and type packs. Reaching a class as a base or owner contributes its namespace.
  A separate occurrence of its type expands its arguments. Member-pointer values still need #411.
- Ordinary calls also associate the declaring namespace of a namespace-scope type
  template supplied as a template-template argument ([basic.lookup.argdep]). Clang mode
  skips alias template arguments in this namespace walk. GCC mode uses the original
  template's identity and namespace when an unconstrained alias forwards the entire
  matching parameter list and preserves the defaults.
- Each associated namespace search uses preceding declarations that bind names in the scope.
  It uses namespace using-declarations' fixed targets and the latest redeclaration found
  for each function. Using-directives do not contribute to ADL. Sema merges the candidates with
  ordinary lookup before overload resolution, then deduces the selected function's return type
  if needed. Class members and nonfunctions suppress ADL. A block using-declaration that names
  a namespace function permits ADL. Qualified and parenthesized calls use ordinary lookup.
- A reachable associated class contributes its ordinary unqualified friend function
  declarations, including declarations whose function is defined in another class. A matching
  namespace declaration binds the name for ordinary lookup at its own point. While reading a
  class member declaration, ADL can use earlier friend signatures from that class.
  [#259](https://github.com/yrashk/ycxx/issues/259)
- A nondependent call in a template uses its lexical lookup point. Dependent calls also use the
  existing point of instantiation ([temp.dep.candidate]). Per-name incremental scheduling
  remains under #16.
- Supported ordinary free binary operators use the full ADL association set, including base
  namespaces and hidden friends. Namespace using-directives do not contribute candidates.
  Supported dependent binary operator expressions on ordinary non-template class operands retain
  definition-time ordinary lookup and add eligible associated nonmember candidates at instantiation.
  [#357](https://github.com/yrashk/ycxx/issues/357)
- Remaining work in #165 includes declarations of functions in blocks, exact lookup for
  other operators, member templates as template arguments, dependent-call template argument associations,
  overload-set arguments and complete unqualified template-id lookup.
- Tests: `tests/own/argument-dependent-lookup/` and `tests/own/range-for/adl*.run.cpp`.

## `<cstdio>`, step 3: attributes, asm labels, string-initialized arrays (#133, 2026-11-18)

- **Attributes:** `[[…]]` attribute-specifiers on declarations, class heads and statements, and
  GNU `__attribute__((…))` before and after declarators, are parsed. The parser skips
  unrecognized attributes ([dcl.attr.grammar]/6) and diagnoses a standard attribute before a
  linkage-specification with a single declaration ([dcl.link]). GNU hidden visibility on ordinary
  external namespace-scope functions and non-inline variables is implemented for x86-64 ELF.
  [#602](https://github.com/yrashk/ycxx/issues/602) · [#936](https://github.com/yrashk/ycxx/issues/936)
- **Asm labels:** `__asm("…")` after a declarator gives the entity's symbol exactly as written
  (`Program::asm_label`, `pipeline::Entity::asm_label`), as the SDK's `__DARWIN_ALIAS` needs.
  Both extensions are listed in `docs/EXTENSIONS.md` (DECISIONS §1.2).
- **Arrays:** character arrays from string literals, with or without braces ([dcl.init.string]),
  their bound from the literal when it is left out; `extern T a[];`, an incomplete type
  (`Types::array_unknown`), completed by a later declaration with a bound ([basic.link]/11);
  arrays of pointers of unknown bound.
- Unknown bounds survive abstract and grouped declarators, including pointer and reference
  parameters. A parameter whose completed type is an array still adjusts to a pointer. Template
  substitution preserves the omitted bound, so partial specializations match `T[]`
  ([dcl.array], [dcl.fct], [temp.deduct.type], #222). The Itanium `A_` form carries omitted
  bounds into ELF and Mach-O symbol names.
- `__FLT_EVAL_METHOD__` is predefined (0), as Clang has it for the target.
- Attributes are also accepted after a declarator-id, on parameters, alias-declarations and
  statements, and in the GNU form on class heads and after template-heads. A string literal
  initializes a character-array element of an aggregate. Uses of default member initializers
  outside the supported cases are diagnosed (#118).
- Leading standard attributes on ordinary data members and member functions are accepted
  ([class.mem.general], #190). The parser diagnoses unbalanced arguments in these prefixes
  ([dcl.attr.grammar]) and attributes before access labels ([class.access.spec]). Other attribute
  placements still have the balance gap tracked in #190. Attribute effects remain tracked in #305.
- Tests: `tests/own/decls` (11, three from the review). Five more draft examples pass.
- Not yet: `sizeof`, `alignof`, `decltype`, `static_cast` and other casts, operator functions,
  unions (the next steps of #133); asm labels on class members and the other gaps of #190.

## `<cstdio>`, step 4a: casts, sizeof, alignof, decltype (#133, 2026-11-18)

- **Casts:** `static_cast` ([expr.static.cast]): the implicit conversions, to `void`, between
  integers and enumerations (scoped ones too), and from `cv void*` to an object pointer;
  functional casts `T(e)` for keyword types, enumerations and typedef-names, and C-style casts
  `(T)e`, as static_cast does them ([expr.type.conv]/2, [expr.cast]/4).
- Runtime `static_cast<D*>`, `static_cast<D&>` and `static_cast<D&&>` from a base xvalue
  undo an accessible, unambiguous nonvirtual base conversion to complete `D`. They preserve
  null pointers and the reference operand's storage and value category. The target can add
  cv-qualifiers. Rvalue-reference downcasts from lvalues, C-style and functional downcasts, and the
  const_cast and reinterpret_cast parts of C-style casts are tracked in
  [#199](https://github.com/yrashk/ycxx/issues/199). Constant evaluation of a base
  adjustment in a downcast is tracked in [#102](https://github.com/yrashk/ycxx/issues/102).
- `static_cast<T&&>` gives a compatible object lvalue the target type and xvalue category for
  reference binding, `decltype` and overload resolution. The cast designates the same object or
  an accessible, unambiguous base subobject. A compatible bit-field is read into a temporary.
  The tests in `tests/own/casts` cover identity, qualification, overloads and direct reference
  initialization. [#199](https://github.com/yrashk/ycxx/issues/199)
- Named `const_cast` changes cv-qualification on ordinary pointers with matching shapes,
  including cv `void*`, and on scalar lvalues and xvalues ([expr.const.cast],
  [#1424](https://github.com/yrashk/ycxx/issues/1424)). It keeps the original address and
  gives a reference cast the requested value category. In a constant expression, an access
  still respects the qualifiers of the underlying object. A by-value class parameter object uses
  the definition's declared qualifiers ([dcl.fct]/5) and permits writes to mutable members.
  Pointer-to-member casts and class or array reference casts are tracked in [#199](https://github.com/yrashk/ycxx/issues/199).
- Named `reinterpret_cast` converts object pointers (including cv `void*`) to pointer-width
  integral types and back, object pointers to other object pointers, function pointers to
  other function pointers and to and from object pointers, and integral, enumeration and
  pointer values to their own type. It converts an object glvalue to a reference to another
  object type, naming the same object with the requested value category. These work in
  templates, with dependent or non-dependent operands. Each conversion keeps the address, and
  casting away constness ([expr.const.cast]/7), a prvalue or bit-field to a reference, or a
  too-narrow integer destination is ill-formed. A reached cast is disallowed in a core constant
  expression, and `-fcompat=gcc` folds a cast to the operand's own type, as GCC does.
  Pointer-to-member, `std::nullptr_t` and enumeration or non-pointer-width integer to pointer
  conversions, and function references, remain in [#199](https://github.com/yrashk/ycxx/issues/199).
- **sizeof, alignof** of fundamental, pointer, enumeration and array types and of an expression,
  from `ast::Target`; a class's since #194, below.
- **decltype(e)** ([dcl.type.decltype]): an unparenthesized id-expression or member access names
  its declared type, else the expression's type and value category. `decltype(auto)` is not
  supported yet.
- `std::nullptr_t` objects are laid out as pointers. Equality and inequality evaluate both
  operands and support two `std::nullptr_t` values or one pointer and one `std::nullptr_t`
  value ([expr.eq]). The lvalue-to-rvalue conversion evaluates a `std::nullptr_t` glvalue
  without reading its object, including during constant evaluation ([conv.lval],
  [expr.const.core], CWG 2907). This includes conversion of ordinary automatic objects
  during their own initialization and from an enclosing block. The semantics of the
  `[[indeterminate]]` attribute are tracked in [#305](https://github.com/yrashk/ycxx/issues/305).
  Other conversion and cast gaps are tracked in [#199](https://github.com/yrashk/ycxx/issues/199).
- A literal can be folded to a fixed enumeration's constant. The constant evaluator checks
  casts to non-fixed enumerations against their value range (#262). `decltype` reads declared types from
  the declaration that named the entity; a statement that begins with a functional cast is an
  expression unless it can be a declaration ([stmt.ambig]).
- Tests: `tests/own/casts` (9, three from the review). One more draft example passes.

## `<cstdio>`, step 4b: unions, floating types as types, include directories (#133, 2026-11-18)

- **Unions** ([class.union]): `union U { … }`, named, unnamed with a typedef-name, as members
  and through `union U` elaborated; every member at offset 0 and the size of the largest
  (`ClassRec::union_`, `ClassShape::union_`, `QueryLayout::class_layout`); a braced initializer
  initializes the first member ([dcl.init.aggr]/16), with its default member initializer if it has
  one; class template unions. A reference member is rejected. A designator naming another member,
  a default member initializer on another member and anonymous unions are tracked in #203.
- **Floating types as types:** `float`, `double` and `long double` in decl-specifiers, so the
  SDK's `typedef long double max_align_t;` declares; `sizeof`/`alignof` give their sizes. Their
  values (literals, conversions, arithmetic) are #201 and are diagnosed, not miscompiled.
  `__STDCPP_FLOAT16_T__` and the other extended floating-point macros are no longer defined
  until those types work (DECISIONS §1.5; Clang does not define them either).
- **Include directories:** a `-I` or `-isystem` directory that is one of the built-in system
  directories is searched only at the system directory's place, as GCC and Clang do; before,
  `-isystem deps/libycxx/include` made libycxx's `<cstddef>` find its own `<stddef.h>` again.
- A floating local no longer stops constant evaluation in its function (the thunk skips it).
- Tests: `tests/own/unions` (7, three from the review). Six more draft examples pass.

## Unnamed union types in named data members (#203)

- A non-template class can declare a data member with an unnamed union type as in glibc's `__mbstate_t`.
  The nested type uses ordinary union layout and access through the named member. Declarators
  following one union definition share its type.
- Astra recommended the enclosing class and lexical unnamed-type ordinal as the type's identity
  across translation units. The splitter distinguishes identical sibling definitions and their
  members with the ordinal, and the Itanium mangler uses it for `Ut_`, `Ut0_`, and later names.
- `unit.items` checks the splitter's ordinals and item identities. `unit.pipeline_incremental`
  checks layout, member lookup, mangling, and cold and warm builds after swapping identical
  sibling unions.
- Anonymous union member injection and the other union extensions remain tracked in #203.

## Nontrivial union members with a written destructor (#203)

- An ordinary named union can contain a nontrivially destructible member with a trivial or
  deleted copy constructor if the union declares a destructor. Braced aggregate initialization
  can initialize the first member. A written constructor initializes its chosen member or
  leaves the union without an active member. Initializing two members is ill-formed.
- A union destructor runs its own body and leaves variant members to explicit lifetime
  management ([class.dtor]). Tests in `tests/own/unions/` check constructor selection and
  observe the union and member destructor effects independently.
- An omitted aggregate member of union type is initialized from an empty braced list,
  so its first variant can run a constructor.
- In GCC mode, a direct empty union allocation walks ordinary variant members and bases.
  A proven public trivial defaulted constructor leaves the enclosing variant eligible,
  while a user-provided default constructor still triggers the deleted-default diagnostic
  without a default member initializer. Nested union declarations and initializers on
  later variants remain in #203. [#1483](https://github.com/yrashk/ycxx/issues/1483)
- A union constructor that selects a nontrivially destructible member can use the hosted
  Linux cleanup subset above or a nonthrowing exception specification. A potentially
  throwing delegating constructor with a nontrivial variant receives a diagnostic.
  Broader exceptional cleanup remains tracked in #89 and #135.
- Remaining implicit special-member rules and aggregate designator forms stay in #203.
  Direct explicit destructor calls are tracked in #722.

## Concrete nontrivial members of union templates (#203)

- A namespace-scope union template with a written destructor can instantiate nontrivially
  destructible variants with trivial or deleted copy constructors. Nonthrowing constructors
  select the active member, and the union destructor leaves variant cleanup explicit.
- The instantiation checks the concrete variant types and the completed destructor declaration.
  Tests select both members of a union specialized on a noncopyable resource and diagnose
  the unsupported implicit-destruction form.

## `<cstdio>`, step 4c: operator functions; `<stdio.h>` compiles (#133, 2026-11-18)

- **Operator functions** ([over.oper]): `operator@` declarator-ids for the overloadable
  punctuators, `()` and `[]`, at namespace scope and as members; a non-member one needs a class or
  enumeration parameter and the operator's arity, and `=`, `()`, `[]`, `->` are members only.
  Itanium `<operator-name>` codes (`or`, `oR`, `co`, `ng`/`mi` by arity), checked against Clang's
  symbols.
- **[over.match.oper]:** a unary, binary or compound assignment operator with an operand of
  class or enumeration type calls `operator@` when one is viable: the left operand's member, else
  the non-members unqualified lookup and argument-dependent lookup find; else the built-in
  operator. `a = b` calls a user-declared member `operator=`. Ranking against the built-in
  candidates, postfix `++`/`--`, `()`, `[]`, `->` and conversion functions are #219.
- Ordinary constructors support concrete `noexcept` and `noexcept(e)` specifications.
  Out-of-class definitions check the effective specification of the corresponding declaration.
  Omission agrees with `noexcept(false)` on ordinary potentially throwing constructors, and
  bare `noexcept` agrees with `noexcept(true)` (#219).
- `N::T(…)` and `N::T{…}` are functional casts.
- `T (*p)(args);` with T a typedef-name declares p ([stmt.ambig]), in sema and in the splitter.
- **`#include <stdio.h>`** (the SDK's) compiles, and `printf`, `puts`, `fputs` and `stdout` run
  (`tests/own/headers/stdio.run.cpp`). `<cstdio>` still waits for static members of class
  templates and template template parameters (#88) and pointer-to-member types (#238).
- Member operators get the [over.oper] checks too; `S::operator@` and `N::operator@` define outside;
  unary `*` and `&` call operator functions; a deleted or ambiguous choice is an error, not the
  built-in operator. `N::T(…)` is a cast only when no function or variable hides N::T.
- Ordinary nonmember operators can be called explicitly as `operator+(a, b)`, `::operator+(a, b)`
  or `N::operator+(a, b)`. Unqualified calls use ordinary lookup and argument-dependent lookup.
  Qualified calls use namespace lookup. An unqualified member operator name can also use the
  implicit object, including in a lambda that captures `this`. [#219](https://github.com/yrashk/ycxx/issues/219)
- Tests: `tests/own/operators` and `tests/own/headers`.

## Cooked integer user-defined literals (#289, 2026-11-20)

- A literal operator name has its own lookup spelling derived from the suffix identifier
  ([over.literal]). Compact and deprecated separated spellings name the same function.
- An integer user-defined literal uses ordinary unqualified lookup at its point of use. When
  the visible set contains an operator with an `unsigned long long` parameter, the existing
  integer conversion rules build the argument and ordinary overload resolution builds the
  call ([lex.ext], [lex.icon]). The call can run or be evaluated as a constant expression.
- The Itanium mangler emits the `li` operator code followed by the suffix's source-name.
  Tests in `tests/own/cooked-literals` cover lookup, integer forms, rejection, and a peer object.
- #289 tracks the remaining literal call forms, including raw and numeric template calls.

## Object model, step 1: classes by value (#116, 2026-11-18)

- **L1:** a parameter of class type is the address of an object the caller initializes
  ([expr.call]/7); sema materializes the argument, so the caller destroys it at the end of the
  full-expression (Itanium C++ ABI §3.1.2). A class result is written through a hidden argument,
  the address of the object the call initializes ([stmt.return]): the caller's variable, member or
  temporary, so returning a prvalue copies nothing. The call op's two new immediates and
  `ir::Function::byval_params`/`result_param` mark both. A discarded class result is a temporary
  ([expr.context]/2); `f({…})` with one candidate and `return {…};` copy-list-initialize.
- **`lower`** applies AAPCS64 (§6.8.2 C.10–C.12, §6.9): a class trivially copyable for calls
  ([class.temporary]/3, `sema::Classes::trivial_for_calls`, `TypeLayout::trivial_calls`) of at
  most 16 bytes passes and returns in one or two 8-byte registers; a larger or non-trivial one by
  address, an indirect result in x8 (`l2::Function::indirect_result`, the call's fourth
  immediate). A class needing more registers than are left goes whole on the stack and leaves the
  rest unused (C.11). Spec and Clang modes omit the tested empty-class arguments on Darwin.
  GCC mode transports a naturally aligned one-byte empty argument in direct calls on Darwin.
  The published Itanium rule selects which classes are trivial for calls by default. On x86-64 Linux, GCC mode passes
  supported classes whose copy is implicitly deleted by a class subobject in stack slots.
  The explicit
  `-fclass-call-abi=itanium` and `-fclass-call-abi=apple` flags override that choice (DECISIONS §1.6).
- **Mixed-object tests:** `PEER:` in a run test names a file the oracle Clang compiles and links
  with it, or Apple Clang with `PEER-WITH: apple-clang` (docs/TESTING.md). `tests/own/abi`
  checks 3- to 24-byte classes, member functions returning classes, exhausted registers, empty
  classes, and each kind of class for calls (trivially copyable, non-trivial copy or destructor,
  move-only, deleted copy, a move-only member) under both settings, calling both ways.
- Class arguments through an ellipsis and calls through pointers returning classes remain in
  #196, and classes in constant evaluation remain in #25. (Classes with floating members: #336.)
- Tests: `tests/own/abi` (5), `tests/own/classes/s5-byvalue-lifetimes`, `-temporaries`;
  s1-byvalue-params and -return pass.

## Class prvalues returned by calls (#679)

- Ordinary class-valued calls preserve the result's top-level `const` and `volatile`
  qualifiers ([expr.call], [expr.type]). The tests in `tests/own/call-result-cv/` cover
  `decltype`, overload selection, and the existing adjustments for scalar and reference
  results.

## Scalar branch temporaries (#902)

- Scalar logical and conditional expressions carry each selected reference-argument temporary
  to the end of its full-expression ([class.temporary], [expr.log.and], [expr.log.or], [expr.cond]).
  Both the interpreter and native code use the L1 join with nullable temporary addresses. Conditional
  nontrivial destruction and reference lifetime extension remain under [#157](https://github.com/yrashk/ycxx/issues/157).

## Class prvalue selection (#740)

- The built-in `?:` or comma operator can initialize an automatic class object, an ordinary
  aggregate field, or a fixed-array element directly ([expr.cond], [expr.comma], [dcl.init.aggr],
  [dcl.init.general]). The conditional evaluates one arm. Comma evaluates its left operand
  before initializing from its right operand. The selected expression uses the destination's
  storage, including for types with deleted copies and moves.
- The result starts its lifetime before cleanup. Temporaries from the selected arm survive
  later initializer clauses and are destroyed with the rest of the full-expression's
  temporaries in reverse order ([class.temporary]). Tests in `tests/own/class-selection/`
  check both arms, later sibling initialization, and temporary and object destruction.
- A native by-value return can initialize its result object from a built-in conditional
  with nonthrowing evaluation and cleanup. In the supported mixed case, overload
  resolution copy-initializes a prvalue from the cv-unqualified lvalue arm of the same
  class. A selected prvalue can initialize the result even when copy and move are deleted.
  The result's lifetime starts before temporary destruction, which precedes destruction
  of the function's locals ([stmt.return], [basic.life]).
- This slice supports nonthrowing initializers and cleanup. More general cleanup during
  unwinding remains in #219. Potentially overlapping subobjects remain under #301 and #304,
  other class initializer contexts under #740, and other conditional temporaries under #157.

## Braced arguments in free function overload resolution (#231, 2026-11-20)

- Free non-template candidates can rank a braced argument as a conversion sequence. An empty
  list has identity rank for an arithmetic parameter and a single expression uses its ordinary
  sequence. A flat positional arithmetic list for an ordinary aggregate with public arithmetic
  fields contributes one user-defined sequence, ranked ahead of ellipsis ([over.ics.list],
  [over.ics.rank]). Typed sibling arguments still participate in ADL.
- The selected function's arguments use the existing list-initialization path, which checks
  narrowing and initializes omitted fields. A dependent list defers call analysis. Candidates
  with unsupported conversions that remain plausible after checking arity and other arguments
  produce an unsupported diagnostic.
- A braced argument for a selected class reference parameter binds to a single reference-related
  expression or list-initializes a temporary of the referenced class. The temporary lives until
  the end of the full-expression containing the call ([dcl.init.list], [class.temporary], #503).
- Constructors, member overloads, template deduction, class-valued clauses, and general
  reference, array, aggregate, and `std::initializer_list` cases remain under #231.
- Tests: `tests/own/braced-overloads/`.

## `std::initializer_list` backing array (#329)

- A direct call with a braced argument and a `const std::initializer_list<int>&` parameter
  initializes a temporary `const int[N]` in clause order and checks narrowing. The libycxx list
  views that array until the end of the full-expression containing the call ([dcl.init.list],
  [class.temporary]). The empty argument uses the list's default constructor.
- Braced by-value calls, named nonempty lists, other element types and braced ranges remain in
  #329. General list-conversion ranking remains in #231.
- Tests: `unit.linux_compile` with the pinned libycxx header.

## Templates, ζ step 1: function templates (#88, 2026-11-18)

Instantiation follows ast.md §6.4 as amended by #88 (Fable's decision): declarations and
canonical positions are substituted structurally; bodies are instantiated by analysing the
template's tokens again with the arguments bound.

- **Types and records:** `TTemplateParam{depth, index, pack}` in `types`; `TemplateArg` and
  `TemplateParamSema` in `seqs`; `DeclSema::template_params` (a template's head) and
  `decl_flag::template_`. Sema's stand-in entities gained `function_template` (identity: scope,
  name, type and the head's kinds) and specializations (`templ` and canonical arguments, §B1.5).
- **Parsing:** template-heads on namespace-scope function declarations (type parameters,
  integral, pointer and reference non-type parameters, defaults); template-ids `f<args>` in
  expressions, with `>` ending the list outside brackets ([temp.names]/4).
- **Phase one** ([temp.res]): a template's body is analysed with its parameters in scope; a
  type- or value-dependent construct becomes a `Dependent` node (operands as written), an
  unqualified callee lookup did not find a `DepName`, and the body records which unqualified
  calls were dependent ([temp.dep.candidate]). Non-dependent errors are reported here.
- **`ycxx.sema:templates`:** `substitute` (with the failures of [temp.deduct.general]/11),
  deduction from a call ([temp.deduct.call], [temp.deduct.type]: decay, forwarding references,
  the cv and qualification allowances of /4), explicit arguments converted to their parameters'
  types, default template arguments, and partial ordering ([temp.func.order],
  [temp.deduct.partial]).
- **Overload resolution:** a template contributes the specialization deduction gives; a
  non-template beats an equally good specialization, and the more specialized template wins
  ([over.match.best]/2.4–2.5).
- **Instantiation at the end of the unit** (§B6.2), in `parse::Program` until contextual queries
  (#16): cells carry `Demand`s, and `instantiate_all` walks them in item order, then each
  instance's, to a nesting limit of 900. The body is re-analysed over the template's tokens
  (`BodyAnalysis` with an `Instantiation`): unqualified names at the template's item, the
  dependent calls' argument-dependent lookup, completeness and constant evaluation at the end
  (`AtInstantiation`). A specialization needed by constant evaluation is instantiated where it is
  needed ([temp.inst]/8). Diagnostics point into the template.
- **Mangling:** `<template-args>` (types, integers as `L…E`), `<template-param>`s and the return
  type in a specialization's name, and function types (`F…E`); checked against Clang's symbols.
- Dependent array bounds `T (&)[N]` and `int a[N]`, with `N` deduced from an array argument
  ([temp.deduct.type]/8) and mangled as `A T_ _`; the reference tie-break of
  [temp.deduct.partial]/9. Reference binding reads an array's cv-qualifiers from its elements
  ([basic.type.qualifier]/3), for templates and other code.
- A whole `sizeof(type-id)` bound with a dependent type operand stays dependent in a
  class template's non-static array member. Completing a specialization replays the member's
  declaration and checks
  the ordinary layout and bound there. Merely naming a pointer to the specialization leaves
  the operand unchecked until a completion demand ([dcl.array], [expr.sizeof], [temp.inst],
  [#1191](https://github.com/yrashk/ycxx/issues/1191)).
- Tests: `tests/own/templates/f1-*` (spec-only, plus ten from the review): 47 of 48 pass; the
  other calls through a function pointer (#24). 9 more draft examples pass ([temp.over], [temp.deduct.general],
  [temp.deduct.partial], [temp.arg.general], [temp.deduct.type]).
- Not yet (#88): class templates, specializations, packs, concepts, member templates, qualified
  template-ids (`N::f<int>`), a non-type parameter's default or a signature's bound that is a
  dependent expression other than a parameter (not supported, pending `VDepOp`), dependent
  qualified names (`typename T::x`, with the `SSubst` sugar of ast.md §6.4), deduction from an
  overload set argument ([temp.deduct.call]/6), "required from here" notes. Coalesced (weak)
  definitions of specializations are #124.

## Derived-base function template deduction (#1144)

When ordinary deduction fails for a fixed class-reference or single-pointer parameter
with a named class-template pattern, deduction examines concrete bases at the call site.
A unique most-derived matching specialization supplies the arguments, and the normal
conversion checks the selected call. By-value deduction and conversion ranking through
repeated nonvirtual base subobjects remain in [#1144](https://github.com/yrashk/ycxx/issues/1144).

## Native function pointer template arguments (#406)

- A function pointer template argument can name an unambiguous externally linked C++ or C
  function. The bare function name and its address identify the same specialization.
  A C++ pointer type can come from an earlier type argument, including a `noexcept`
  signature. A null pointer to a C++ function type has its own specialization.
- Linkage names use the symbolic entity from constant evaluation. The function encoding
  shares the enclosing name's Itanium substitutions. Native calls use the same entity
  for the relocated address on x86-64 Linux and arm64 Darwin. A potentially throwing
  pointer initialized from a `noexcept` function preserves its pointer conversion
  in the specialization name.
- Tests: `tests/own/templates/function-pointer-arguments/`. Function references and
  target-typed selection from overload sets stay in #406. Directly taking the address
  of a function template specialization as an expression stays in #335.
  Mangling a C-linkage function type stays in #287.

## Native object pointer template arguments (#1055)

- A native symbol can encode a pointer template argument naming a whole namespace-scope
  object or a static data member of an ordinary namespace-scope class. Mangling uses the
  symbolic object from constant evaluation, with the enclosing name's Itanium substitutions.
- Fixed, directly matched pointer parameters of class templates can also take a whole object
  converted to cv `void*`, and a completed array whose first declaration had unknown bound.
  The two native targets use the same entity-literal spelling as both peer compilers in
  these cases. Other template argument contexts remain in #1073.
  [#1070](https://github.com/yrashk/ycxx/issues/1070)
- On both native targets, members specialized for pointers to distinct TU-local objects
  keep their symbols and storage separate across translation units. Externally linked object
  arguments preserve their identity when separate objects refer to the same specialization.
  [#1055](https://github.com/yrashk/ycxx/issues/1055)

## Value-dependent template arguments in symbols (#773)

- A function template whose signature holds a value-dependent template argument has a symbol:
  `X <expression> E`, with a dependent variable-template-id as its `<source-name>` and
  `<template-args>` and `!e` as `nt e` (Itanium C++ ABI §5.1.6). `std::move_if_noexcept`'s
  return type needs it for any libycxx header in an object.
- The default and Clang modes use the written arguments and unresolved-name qualifiers for the
  supported forms. GCC mode uses the converted arguments and unqualified name (DECISIONS §1.7).
- Tests: `tests/own/abi/dependent-template-args.run.cpp`, linked with a Clang peer that calls the
  specializations. `&&` follows #671; type-trait operations (`__is_same(T, U)`) still have no
  symbol (#780).

## Concept-ids and comparisons as dependent template arguments (#1548)

- A concept-id with dependent arguments read as an evaluated template argument is a
  `VDepConcept` value in every declaration context, unqualified or namespace-qualified, and under
  `!` ([temp.names]/8, [temp.dep.constexpr]/2). Substitution evaluates it once the arguments are
  known (`Templates::substitute_concept_value`). `typename cond<C<I>, …>::type` in an alias
  template, a member alias or a function signature works, as libycxx's `iter_difference_t` needs.
- In a function signature it is `X <expression> E` (Itanium C++ ABI §5.1.6), GCC's form in every
  mode. Clang qualifies a concept outside the global namespace (`dependent-concept-symbol` in
  `docs/compatibility.toml`, #1589).
- A dependent value with no `VDep*` form, such as `(sizeof(I) > 4)` or `I == 0`, gets
  `template_value`'s placeholder in a namespace alias template and in an ordinary member alias of
  a class template, as in a class base. The alias is parsed again with its arguments bound
  (`AliasCallPattern::Kind::unrepresented`, `AtItem::replay_alias_calls`), the member alias with
  its class. Named with dependent arguments, such an alias is a `TDeferredAlias`: its identity is
  the alias and its arguments, not the shared placeholder, so `gt<T> f(T)` and `lt<T> f(T)` are
  different templates. Substituting concrete arguments replays the alias, also for a function
  specialization's parameter types; deduction binds a bare parameter to the deferred alias itself,
  which keeps partial ordering of a partial specialization like `P<T, gt<T>>` exact. Two
  different aliases with token-identical comparisons stay distinct, though [temp.over.link]/5 makes
  them equivalent (#209). A signature through such an alias has no symbol yet. Member alias
  templates and function signatures that write the comparison directly still reject it (#209).
- `C<I> && D<I>` of concept-ids is a `VDepOp` conjunction, as for variable templates; `||` waits for
  the replay. A conjunction in a signature has no symbol yet (#209).
- Tests: `tests/own/templates/concept-id-alias-argument.{accept,reject}.cpp`,
  `tests/own/templates/dependent-comparison-alias-{argument,identity}.accept.cpp`,
  `tests/own/abi/dependent-concept-args.run.cpp` (linked with a Clang peer).

## Enumeration-typed template parameters (#774)

- A non-type template parameter may have an enumeration type ([temp.param]/7), scoped or
  unscoped, with a fixed underlying type, in a namespace. Its arguments are converted constant
  expressions of that type ([temp.arg.nontype]), so an integer is rejected. For an integral
  parameter, an unscoped enumeration converts through its underlying type, without narrowing, and a
  scoped one does not convert. Explicit and partial specializations, defaults and deduction from a
  specialization work.
- Symbols write an argument as `L <enumeration type> <value> E`, with `n` before a negative value,
  as Clang and GCC do (`tests/own/abi/enum-template-args.run.cpp`, with a Clang peer).
- Tests: `tests/own/templates/enum-parameters/`. A functional cast as a template argument,
  `h<E(5)>()`, is an expression (#810, `tests/own/templates/functional-cast-argument-tentative.accept.cpp`).

## Placeholder non-type template parameters (#1244)

- `template<auto X>`, `template<decltype(auto) X>` and `auto... Xs` ([temp.param]/7). Each
  argument, written, defaulted or deduced through a specialization or an array bound, gives the
  parameter its own type without cv ([temp.arg.nontype]/1, [dcl.type.auto.deduct]): integral,
  enumeration, pointer, `std::nullptr_t` or a flat structural class. That type is part of the
  argument, so `v<1>` (`int`) and `v<sizeof(char)>` (`std::size_t`) are different
  specializations ([temp.type]).
- Inside the template the parameter's type is its own position's `TTemplateParam`, which
  substitution reads from the bound value, so `decltype(X)` is dependent and later parameters and
  defaults may use it. As an independent parameter type it is a non-deduced context.
  `TemplateParamSema::placeholder` records `auto` or `decltype(auto)` for the template-head's
  identity and for symbols, which write `TnDa`/`TnDc` (`TpTnDa` for a pack) before the argument, as
  Clang and GCC do (GCC writes none for an empty pack).
- Tests: `tests/own/templates/placeholder-parameters/`, with a Clang peer for the symbols.
- Not yet: `C auto X` (#286), `auto* P`, `auto& R`, and `decltype(auto)` of a parenthesized lvalue
  or a reference, which deduces the prvalue's type instead of a reference (#1401). A floating-point
  argument is rejected (#317), and a `nullptr` argument has no symbol yet (#64). `decltype(X)` in
  a function template's signature is mangled as `T_`, not `DtT_E` (#1406), and an alias template
  passes `I<'a'>` on as a `char` where its pattern's parameter is `int` (#1405).

## Pack-index-specifiers (#297)

- `T...[I]` ([dcl.type.pack.index]) names the element of a template type parameter pack, with
  `I` converted to std::size_t. It works in alias templates, member aliases, function signatures
  (leading and trailing return types, parameters), variable templates, bodies, type-trait
  operands and functional conversions (`T...[I](x)`). The index is an integral constant or a
  non-type parameter.
- While it is dependent it is the type `TPackIndex` (ast.md §7.6): over the pack's parameter, or
  over its elements once the pack is bound and the index is not (a member template indexing its
  class's pack). Substitution selects the element, also past known elements into another pack's
  expansion (`F<T...>` for `using F = U...[0]`). It is a non-deduced context
  ([temp.deduct.type]/5), and partial ordering indexes a pack's unique stand-in.
- An index out of range, a negative or non-integral index and a name that is not a pack are
  diagnosed.
- In symbols, `Dy <type> <expression>` is a substitution candidate. Spec and GCC modes use the
  converted constant index and the bound pack as `J <args> E`. Clang mode preserves a written
  literal's type and uses a placeholder for a bound pack (DECISIONS §1.7).
- GCC and Clang modes retain the written identity of supported named indices and operator
  expressions. Spec mode uses the converted value. A named enumerator keeps its enum type in
  compiler modes, and a named constant uses an entity reference.
- Tests: `tests/own/templates/pack-index/`, `tests/own/abi/pack-index-*` and
  `tests/daemon/compatibility.py`. The cross-target and native mixed-object checks cover written
  and literal indices, repeated substitutions, and class-bound packs.
- Not yet: pack-index-expressions `t...[I]`, `T...[I]` as a nested-name-specifier
  (`T...[0]::type`) (#297), and dependent index arithmetic such as `T...[sizeof...(T) - 1]` in a
  signature (#209). In a body that arithmetic works.

## Functional conversions with pack expansions (#915)

- `T(a...)` expands its arguments as a call does ([expr.type.conv], [temp.variadic]/5). This is
  also the path a call through a still-dependent alias's member takes at the definition, as in
  libycxx's `invoke`. Test: `tests/own/templates/functional-cast-pack.run.cpp`.

## Pack expansions in mem-initializers (#1400)

- A mem-initializer's parenthesized expression-list reads each clause as an expandable operand,
  as a call's arguments are: `value(static_cast<A&&>(args)...)`, `m(1, args..., 2)`. The braced
  form `value{args...}` already did. An empty expansion value-initializes the member. In the
  definition an unexpanded clause, or a member of dependent type, leaves the initializer
  dependent, and the instantiation checks it ([class.base.init], [temp.variadic]).
- A mem-initializer followed by `...` is a mem-initializer-list expansion. The definition
  records it, and an instantiation reads it once for each element, as `expandable` does. Its
  mem-initializer-id must name a pack of base classes ([class.base.init]); `B(t)...` and
  `m(t)...` are diagnosed, as Clang and GCC do. A base named by a template parameter, `B(x)`,
  `T(t)` or `Ts(ts)...`, is checked in the instantiation; while one is written, the definition
  default-initializes no base. An element that is no direct base is reported by its type.
- Nested expansions: an instantiation's element of an expansion or fold no longer notes its
  packs to the enclosing operand (`Sema::end_element`), and a binary fold's operand probe keeps
  its packs, so `h(g(args...))`, `s((a + ...))` and `id((a + ...))` work in bodies and
  initializers. Packs of different lengths are reported once, without a second error for the
  pattern.
- Tests: `tests/own/mem-initializer-packs/` (spec-only author, plus the regression tests and
  the review's): 11 pass. The base-pack cases are XFAIL: `struct D : Bs...` itself is
  #308. Not yet: `V v(args...)` as a declaration's initializer (#1596), and a template-id
  mem-initializer-id such as libycxx's `__tuple_leaf<_Ip, _Ts>(…)...` or a base named by a
  member alias (#1267).
- `<iostream>` hello (#776): 2,468 errors at `07e6d567` → 2,435. No errors remain at
  `optional.hpp:107`, `:201`, `:205`, `:234`, `tuple.hpp:35`–`49`, `:338`, or `pair.hpp:226`.

## Templates, ζ step 2: class templates (#88, 2026-11-18)

- **Declarations:** class templates at namespace scope (`template<…> struct X {…};` and
  forward declarations), with the defaults rule of [temp.param]/14. The template is a
  `class_template` entity; inside it, `X` and `X<T…>` are the current instantiation, a class
  specialization with the parameters as arguments ([temp.dep.type], [temp.local]), whose record
  is the pattern's. Member items are analysed in the template's head's scope.
- **Template-ids** `X<args>` as types (declarations, parameters, template arguments), in
  expressions (`X<int>(…)`, `X<int>{…}`) and in nested-name-specifiers (`X<T>::f`,
  `N::X<int>`); `>>` closes two lists ([temp.names]/4). Arguments are checked and converted
  against the parameters, with defaults from every declaration ([temp.arg], [temp.param]/12).
  `typename T::x` is its own dependent type (`TDepName`: substituted to the member type, a
  non-deduced context, mangled `N T_ 4x E`), and `T::name` is dependent in phase one
  ([temp.res.general]/5, [temp.dep.type]).
- **Instantiation** (`Program::instantiate_class`, [temp.inst]/2): where a specialization must
  be complete, after its template's definition (not inside it), each member item's declaration
  part is analysed again with the arguments bound and its questions asked at the point of
  instantiation ([temp.point]), and the class record is built whole, then published (#156).
  Each unit keeps its own instantiation state, whichever unit published the record. Nesting is
  limited (`-ftemplate-depth`). Member functions, in the class or defined outside it
  (`template<class T> R X<T>::f() {…}`, constructors and destructors too), are instantiated only
  when an emitted body names them ([temp.inst]/4), found by walking the bodies' trees.
- **Deduction and substitution** through specializations: `const Box<T>&` from `Box<int>`,
  `Arr<N>` deducing `N` ([temp.deduct.type]/8); `substitute` rebuilds `X<T>` as `X<int>`.
- **Mangling:** specializations as `<template-name> <template-args>` with both as substitution
  candidates, inside namespaces too, members as `N…E` (`_ZNK3BoxIiE3getEv`), a value parameter
  as an argument `XT_E`; checked against Clang's symbols.
- Tests: `tests/own/templates/c2-*` (spec-only, plus nine from the review): 44 of 47 pass; the other three need things
  outside templates: constexpr constructors and class objects in constant evaluation (#157,
  #25), aggregate initialization of reference members (#157), and namespace-scope references
  (#367). 11 more draft examples pass. In a daemon session that compiles unrelated programs
  defining one class template differently, the first one's template record wins (#115).
- Not yet (#88): member templates, static data members of
  class templates, explicit and partial specializations, alias templates, packs, concepts,
  qualified names whose nested-name-specifier is a member template, and members found through a
  dependent base.

## Class template argument deduction (#316)

- **Placeholders for a deduced class type** ([dcl.type.class.deduct]): a class template's name
  without arguments, unqualified or `N::C`, in a variable declaration at block or namespace scope
  (`C x(args)`, `C x{…}`, `C x = e`, `C x = {…}`), a functional cast (`C(args)`, `C{…}`) and a
  new-expression (`new C(args)`). The type is a `TAuto` with `deduced_class` set (`Types::deduced_class`),
  allowed only where the parser's caller sets `Parser::deduced_class_ok`. It is the whole declared
  type (not `C*` or `C&`), needs an initializer, deduces the same type for every declarator, and
  is no function's return type.
- **Deduction** (`Sema::deduce_class_type`, [over.match.class.deduct]): the guides are the
  constructors of the primary template's pattern (constructor templates with their parameters
  renumbered after the class's, `constructor_template_guide`), `C()` when C declares no
  constructor or is not defined, the copy deduction candidate `C(C<T…>)`, and the user's
  deduction-guides declared before the point. Overload resolution is `resolve` with the
  tie-breaks of [over.match.best.general]/2.10–2.12 (`Candidate::guide`). Copy-initialization skips
  explicit guides; copy-list-initialization that selects one is an error. While an argument is
  type-dependent the type is dependent.
- **Deduction-guides** ([temp.deduct.guide]) at namespace scope: the splitter marks the item
  (`Item::deduction_guide`, the template-name) and gives it no name; `ItemAnalysis::deduction_guide`
  reads `explicit`/`explicit(cond)`, the parameters, the result (a specialization of the
  template, checked) and a requires-clause, into `Program::deduction_guides_`.
  `Program::deduction_guides` analyses the guide items before the point on demand.
- The splitter reads `S s(&i)` as declaring `s`, not `i`: a `(` after a declarator-id that follows
  a type is never a grouping parenthesis. A template-head's requires-clause ends after a possibly
  qualified concept-id, so `requires C<T> T f(T)` and `requires C<T> W(T) -> W<T>` keep their names.
- Tests: `tests/own/deduction-guides` (12), `tests/unit/items` (`deduction_guides`).
- Not yet: member deduction-guides are accepted and ignored, and deduction through a member
  class template's qualified name (#1706); aggregate deduction candidates and alias
  template deduction (C++20, #1707); the initializer-list phase of [over.match.list] for
  guides (#1708); deduced class types for static data members, non-type template parameters
  and in `for`/condition declarations (#1709); a value default of a constructor
  template's parameter is dropped from its guide.

## Templates, ζ step 3: variable templates and specializations (#88, 2026-11-18)

- **Variable templates** ([temp.pre]): `template<…> [inline] constexpr T v = …;`. The template's
  initializer is a pattern; `v<args>` and `N::v<args>` name the specialization, an entity
  (`variable`, with `templ` and the canonical arguments) like a function template
  specialization. `Program::variable_instance` analyses the template's declaration again with
  the arguments bound, where the specialization is first needed ([temp.inst]/7): in constant
  evaluation (`global`, `constant_value`) at that point, or at the END worklist for code. A
  specialization is emitted as a variable, mangled `_Z1vIiE`, with external linkage even when
  const ([basic.link]/3).
- **Dependent variable-template arguments** (#209): an unqualified namespace variable template
  used as a dependent `bool` template argument keeps the declaration found by ordinary lookup.
  Substitution evaluates the selected specialization once its arguments are concrete.
- **Dependent layout arguments in variable template-ids** (#1371): a visible namespace variable
  template can take `alignof(type-id)` and `sizeof(type-id)` in a variable template initializer or
  a class template's fixed-type inline static member initializer. Replay checks layout at demand,
  converts the target `size_t` result to the declared parameter type and expands the whole query
  in pack order, including repeated elements and empty packs. The same arguments in a direct
  array bound of a non-static field in an in-class nested record replay when that record needs
  to be complete. General dependent alias arguments remain in #209 and placeholder non-type
  parameters in #1244.
- **Integral arithmetic in variable template initializers** (#1091): a direct variable template-id
  such as `extent<T, I - 1>` in a namespace variable template initializer evaluates its argument
  and checks its conversion when the initializer is instantiated with the selected definition's
  bindings ([temp.arg.nontype], [temp.inst]).
  A primary namespace variable template can use this replay in a fixed braced list that
  infers the bound of an array of concrete scalar type. An ellipsis token, a top-level designator,
  or the declared name keeps the list on the earlier parsing path.
  [#1168](https://github.com/yrashk/ycxx/issues/1168)
- **Dependent immediate calls** (#209, [temp.dep.constexpr], [temp.arg.nontype]): an unqualified
  or fixed namespace-qualified call to a uniquely resolved `consteval` function template with
  no function parameters can supply a non-type template argument. The call retains its explicit
  type arguments, including trailing pack expansions, and evaluates to an integer or enum once
  those arguments are concrete.
  Alias type-ids replay calls whose argument validity must be checked at substitution.
  Dependent uses of aliases that can erase the check require signature replay (#209).
- An unqualified call to a single ordinary external C++ immediate function can also supply
  a dependent non-type template argument. The call retains direct integral template parameters
  and integral literals and evaluates the selected function after substitution.
- **Dependent logical negation** (#209): a `bool` template argument or default can negate a
  qualified dependent `bool` variable template-id. Substitution retains the negation across
  aliases until the selected specialization has concrete arguments and supplies a usable
  nonvolatile `bool` constant ([expr.unary.op], [expr.const], [temp.arg.nontype]).
- **Explicit specializations** ([temp.expl.spec]): `template<>` declarations and definitions of
  a class (`template<> struct X<int> {…};`, or only declared, then incomplete), function
  (`template<> R f<args>(…)`, or arguments deduced from the declared type, [temp.deduct.decl],
  the most specialized template when several fit), variable, and member of a class template
  specialization (`template<> R X<int>::f() {…}`). A specialization so declared is never
  instantiated; calls, constant evaluation, completeness and the END worklist use the explicit
  one. A use before the explicit specialization that instantiated it is diagnosed at the end
  ([temp.expl.spec]/7).
  A written nonvirtual destructor can also be explicitly declared and defined on a class template
  specialization. It retains the selected member's identity and gets its inline status from the
  explicit declaration. [#1027](https://github.com/yrashk/ycxx/issues/1027)
- **Partial specializations** ([temp.spec.partial]) of class and variable templates, declared or
  defined. `Program::choose_partial` matches a specialization against those declared before its
  point by deducing their parameters from its arguments ([temp.spec.partial.match]), picks the
  most specialized ([temp.spec.partial.order], the deduction of the rewritten function
  templates), and diagnoses an ambiguity. The partial specialization's members are analysed as
  those of the specialization of its own parameters, and may be defined outside it
  ([temp.spec.partial.member]). Its arguments are checked against [temp.spec.partial.general]/9.1–9.3.
- **Substitution during matching** (#263): the deduced bindings must reproduce the supplied
  arguments. Matching rechecks written dependent `decltype` operands, alias template arguments,
  and access to dependent qualified types. Immediate-context errors discard the candidate
  ([temp.deduct.general]). Instantiations triggered by the check retain their own diagnostics.
  Tests: `tests/own/templates/sfinae-partial-*`. Canonical identity for dependent `decltype`
  operands, including those in alias template definitions, is tracked in #88.
- **Determinism:** before a specialization's pattern is chosen, `specializations_before(point)`
  analyses every explicit and partial specialization item before the point, so the choice does
  not depend on which cells ran first (§A1).
- **Items:** `template<>` is no template-head: `template_depth` counts only real heads, and the
  item and its names carry a `specialization` flag; names of explicit and partial
  specializations are not found by lookup. A class's member items are scoped by its template-id
  (`Kind<T*>`), so a specialization's members have ids of their own.
- Tests: `tests/own/templates/s3-*` (spec-driven, plus ten from the review); 8 more draft examples pass.
- Not yet (#88): alias templates, packs, concepts, member templates and their specializations,
  static data members of class templates, dependent arithmetic template arguments in canonical
  types and signatures (`A<I+5>`), and coalesced definitions of specializations used in two
  units (#124).

## Templates, ζ step 4: parameter packs (#88, 2026-11-18)

*The representation is Fable's decision (ast.md §5.6 and §6.4 amended); the code is the
session's.*

- **Representation:** a pack parameter's argument is a `pack` `TemplateArg` holding its elements
  (`elems`, a list; the empty list is list 0 of every interner), one entry per parameter, so a
  specialization's identity and its Itanium name (`J…E`) keep the grouping. While dependent an
  element may be an expansion: a type `TPackExpansion` or a value `VDepExpansion`; a function
  parameter pack's type is a `TPackExpansion` in its function type. `VDepSizeofPack` is
  `sizeof...(P)` of a template parameter pack.
- **Declarations:** `class... Ts`, `T... Ns` (a pack of an earlier parameter's type too),
  `Ts... xs`, `Ts&&... xs` (`T...` without a pack is a C ellipsis), `pattern...` in template
  argument lists. A class or variable template's pack is last. Spec mode requires function
  template parameters following a pack to be deducible or defaulted ([temp.param]). GCC and
  Clang modes accept those declarations and check whether a call can determine its arguments.
- **Substitution and deduction** (`ycxx.sema:templates`): `substitute_expansion` expands an
  expansion in a list (function parameters, template arguments) with each pack bound to its
  element, failing on packs of different lengths ([temp.variadic]/7). Deduction compares lists
  with packs' elements in place, a trailing expansion against the rest ([temp.deduct.type]/9);
  a call's trailing function parameter pack takes the remaining arguments, forwarding references
  included, after an explicitly specified prefix ([temp.deduct.call]/1, [temp.arg.explicit]/9);
  a pack neither given nor deduced is empty. Partial ordering treats a function parameter pack's
  pattern as the parameter for each remaining argument and prefers a template without a
  trailing pack ([temp.deduct.partial]). Partial specializations match and order through the
  same list deduction (`Head<List<T, Us...>>`).
- **Bodies and initializers** (re-analysis, ast.md §6.4): a definition records its expansions
  (call arguments, braced lists, fold patterns) by their pattern's offset with the packs they
  name; an instantiation reads each pattern once per element with those packs bound to it (a
  stack of frames). A function parameter pack is its elements' parameters under one name. Fold
  expressions of all four forms nest as [temp.variadic] says; an empty one is `true`, `false` or
  `void()` for `&&`, `||` and `,`. `sizeof...` of a template or function parameter pack. A pack
  named outside every expansion, an expansion naming no pack, and an empty fold over another
  operator are diagnosed. A function parameter pack that is not last is not supported yet (#210).
- **Mangling:** `J…E`, `Dp`, `XspT_E`; checked against Clang's symbols.
- Tests: `tests/own/templates/p4-*`; 7 more draft examples pass. `<cstdio>`: 161 → 78 errors.
- Not yet: template template parameters (#208); value-dependent expressions as template
  arguments, which `__int_pack_double` needs (#209); an expansion for non-pack parameters and
  per-element notes (#210); alias templates and concepts (#88); built-in type traits
  (`__is_same`, `__remove_cv`, …) that libycxx's headers use.

## Templates, ζ step 5: alias templates and member aliases (#88, 2026-11-18)

- **Member typedefs and alias-declarations** (#180, item 1): `using T = …;` and `typedef … T;` in
  a class or class template are types of its scope (`ClassRec.types`, as nested enumerations
  are): named by later members and member function bodies, as `C::T` outside, and as
  `typename X<T>::type` through a specialization. In an instantiation, a member is looked up among
  the specialization's own member records (`AtItem::instantiating`).
- **Alias templates** at namespace scope ([temp.alias]): `template<…> using A = type-id;`, an
  `alias_template` entity (scope, name, head). A use `A<args>` (also `N::A<args>`, in
  nested-name-specifiers and wherever a class template-id may be) is the type it stands for with
  the arguments substituted, so it has no identity or mangling of its own; packs group as for
  class templates. `Templates::complete_class` completes a class that substituting
  `typename X<T>::type` needs, instantiating a specialization. A terminal namespace-qualified
  alias specialization such as `N::A<T>` stays a type argument when its expansion is a dependent
  member type ([temp.arg.general], #630).
- Namespace alias substitution checks dependent qualified type arguments written in the alias
  body even when the result discards them. Forwarded aliases retain the check while its bindings
  are dependent. Type requirements and function-template return deduction use the same
  definition-point lookup ([temp.alias], #1311).
- Direct uses of namespace alias-template IDs in non-template code outside requires-expressions
  check the alias template's associated constraints before expanding its type when every argument
  is an explicit non-dependent type. Type-constraints and the template-head's requires-clause
  contribute to the check ([temp.names]/9, [temp.constr.decl]).
  [#1227](https://github.com/yrashk/ycxx/issues/1227)
- Dependent static member calls through class and alias template qualifiers preserve argument
  pack expansions when an ordinary template parameter precedes the pack. The supported cases
  include forwarding empty and mixed lvalue/rvalue packs. A known member type of the current
  instantiation still supports functional construction. [#890](https://github.com/yrashk/ycxx/issues/890)
- Tests: `tests/own/templates/a5-*`; 2 more draft examples pass. `<cstdio>`: 78 → 53 errors.
- Also: `C::T` inside the class (being defined or instantiated) and in its out-of-class member
  definitions names its member type, as the current instantiation's ([temp.dep.type]/1).
- Function type-ids accept a qualified return type (`using F = n::R (*)();`). An identifier
  known to name a type can start the parameter list of an abstract declarator, as in
  `struct S { using F = S(S); };` ([dcl.name], [dcl.fct], [dcl.ambig.res], #216).
- Named function-pointer declarations accept qualified return types, including at block scope
  ([dcl.ptr], [dcl.fct], [stmt.ambig], #433). A grouped pointer can name an existing namespace member,
  and a block declaration can take an unnamed function-pointer parameter (#449).
- Parenthesized direct initialization accepts functional conversions with a braced operand or an
  initial literal, including `S b(S(1));` and `S b(S{1});` ([dcl.ambig.res], #290).
  A later expression can disambiguate a grouped functional conversion, as in
  `Holder h(Tag(n), 2);` ([dcl.ambig.res], #769).
  An additive operator after an identifier inside the operand also selects the initializer,
  as in `Holder h(Tag(n), Tag(n + 1));`. The grouped named and adjusted pointer parameter
  declarations keep their declaration reading ([dcl.ambig.res], #813).
- Qualified functional conversions check access to aliases and typedefs declared directly in an
  ordinary non-template record, for both `C::T(args)` and `C::T{args}`. They use the existing
  member and friend access rules ([expr.type.conv], [class.access.general], #216).
- Access through intermediate alias qualifiers or inherited aliases, dependent lookup and
  expression hiding remain tracked in [#216](https://github.com/yrashk/ycxx/issues/216). Member alias
  templates (`template<…> using __apply = …` in a class), with member
  templates; template template parameters (#208); value-dependent expressions as template
  arguments (#209).

## Type-trait built-ins (#211, 2026-11-18)

- `ycxx.lex:builtins` lists the type-trait built-ins ycxx recognizes (`lex::type_traits`). The
  preprocessor's `__has_builtin` advertises the existing 29 predicates and 13 transformations.
- A predicate `__is_same(T, U)` is a `bool` prvalue constant (`Sema::trait_expr`), value-dependent
  while a type is dependent. A transformation `__remove_cv(T)` is a type specifier
  (`Templates::transform`); of a dependent type it is a `TTransform` that substitution applies and
  deduction treats as a non-deduced context, mangled as Clang does (`u11__remove_cvIT_E`).
- Dependent `__is_same`, `__is_reference`, `__is_const`, `__is_volatile`, `__is_array`,
  `__is_pointer`, `__is_function`, `__is_enum`, `__is_union`, `__is_class`, `__is_member_pointer`,
  `__is_member_object_pointer`, `__is_member_function_pointer`, and `__is_object` in a `bool` template
  argument or default are `VDepOp` values with canonical qualified type operands. Substitution retains
  unresolved operands across aliases and forms valid operand types before evaluating the
  predicate. The ordinary `VInt` result shares the specialization for literal `true` or `false`
  ([temp.alias], [temp.type], [temp.deduct.general], #209).
- Class and union predicates read the parsed class-key from the current translation unit,
  including for forward declarations. A class template specialization's primary supplies
  its kind without instantiation ([temp.class.general], [meta.unary.cat], #209).
- The translation unit's class record retains copy and move eligibility, triviality, and the
  destructor fact for ordinary complete non-template, nonvirtual classes without bases and with
  scalar fields, arrays of scalars, or reference fields. The calculation includes named scalar-only
  unions with a proved destructor. Class subobjects, overloaded special-member kinds, and unions
  needing default-constructor selection give an unknown result ([class.prop], [special], #218).
- A direct `__is_trivially_copyable(T)` produces a `bool` constant for supported concrete
  operands. It peels all array extents, including an unknown outer bound. It accepts existing
  scalar types as trivially copyable and evaluates reference, function and cv `void` types to
  `false`. It queries the class record at the use of the trait, including the original member
  position when a constructor re-reads an initializer. It reports incomplete class elements
  separately from complete classes with unknown facts. A dependent operand can appear in the
  supported single base of a primary class template, including an alias template-id, or in a
  namespace-scope variable template initializer. The selected definition replays the written
  operand at the instantiation demand and retains trait errors at the source spelling. General
  dependent substitution and wider class classification remain in
  [#218](https://github.com/yrashk/ycxx/issues/218).
  [Replay design](docs/design/dependent-trivial-copyability-replay.md)
  `__has_builtin(__is_trivially_copyable)` answers 0 while support is partial ([meta.unary.prop]).
- `__is_trivial(T)` and `__is_pod(T)` give a `bool` prvalue after removing all array extents,
  including an unknown outer bound. They give true for scalar elements and false for references,
  functions and cv `void`. They diagnose incomplete class and enumeration elements. Supported
  class-template bases and variable-template initializers replay dependent operands at demand.
  Complete class and union evaluation depends on the special-member facts in
  [#1303](https://github.com/yrashk/ycxx/issues/1303). `__has_builtin` answers 0 while support is
  partial ([depr.meta.types], [dcl.enum], #1318).
- `__has_unique_object_representations(T)` peels array extents, including an unknown outer
  bound. It returns true for narrow character types, and false for modeled standard floats,
  references, functions and cv `void`. Incomplete class and enum operands have a dedicated
  diagnostic. Complete types with unproved representations have an unsupported-operand
  diagnostic. Supported class-template bases and variable-template initializers replay
  dependent operands and check completeness at demand. `__has_builtin` answers 0 while
  support is partial ([meta.unary.prop], [basic.fundamental], #1062, #218).
- `__is_constructible(T, Args...)`, `__is_nothrow_constructible(T, Args...)` and
  `__is_trivially_constructible(T, Args...)` check concrete operand types at the original source
  point. Their domain includes complete types, cv `void`, and arrays of unknown bound. They fold
  function and cv `void` targets, scalar and reference targets with no arguments, and arithmetic
  targets with arithmetic or arithmetic-reference arguments. Each viable initialization in that
  scope is trivial and nonthrowing. Incomplete operands and other complete combinations get hard
  diagnostics. The supported class-template base and variable-template initializer replay paths
  accept dependent operands. Replay expands packs in place among the arguments, checks newly
  concrete types at the actual demand and retains hard errors at the builtin spelling. An empty
  pack contributes zero arguments. The target must be a separately written type-id.
  `__is_nothrow_constructible(C, C&&)` can use a written exact move constructor and the
  effective destructor facts of a complete ordinary class without bases. A query during class
  completion gets an unsupported-operand diagnostic if the constructor's specification is pending.
  General dependent queries and broader class construction remain in [#218](https://github.com/yrashk/ycxx/issues/218).
  `__has_builtin` answers 0 for all three predicates while support is partial ([meta.unary.prop]).
  [Direct design](docs/design/direct-constructibility.md)
  · [Replay design](docs/design/constructibility-tail-pack-replay.md)
- `__is_assignable(T, U)`, `__is_nothrow_assignable(T, U)` and `__is_trivially_assignable(T, U)`
  fold to the same Boolean when both underlying types are supported arithmetic types. A non-const
  lvalue reference destination is assignable, including a volatile referent. The source may be
  cv-qualified or a reference. Supported class-template base and variable-template initializer
  replay paths evaluate dependent operands at instantiation. `__is_nothrow_assignable(C&, C&&)` can
  use a written exact move assignment returning `C&` in a complete ordinary class without bases.
  `__has_builtin` answers 0 for the three predicates while support is partial
  ([meta.unary.prop], [expr.assign], #218).
- `__is_convertible(From, To)` and `__is_nothrow_convertible(From, To)` share a Boolean result
  for arithmetic values, references to arithmetic types and cv `void`. Two cv `void` operands
  give true. A single cv `void` operand gives false. Arithmetic reference binding accounts for
  value category, the referent's qualifiers and temporary conversion between distinct arithmetic
  types. `__is_convertible` also checks a unique public nonvirtual base path between complete
  class types. It supports direct reference binding and by-value initialization of a fieldless base
  with no bases or written constructors of its own, an eligible implicit trivial copy or move
  constructor, and an eligible implicit trivial destructor. Both traits also answer object
  pointer and `std::nullptr_t` sources converted to a non-reference target: same-type and
  `cv void*` targets with added pointee qualifiers, Derived* to Base* through an accessible,
  unambiguous base of a complete Derived (an incomplete one gives false; virtual functions do not
  matter), the null pointer value, and `bool` targets ([conv.ptr], [conv.qual], [conv.bool]). An
  array pointee's qualifiers are its elements'. Multi-level pointers convert by qualification
  conversions, with each level that adds qualifiers below const ones only, and a pointer to a
  noexcept function converts to one to the plain function ([conv.fctptr]). References to
  arithmetic, pointer and `std::nullptr_t` types follow [dcl.init.ref]/5: direct binding of a
  reference-compatible operand (a qualification conversion of pointers to them), otherwise a
  converted temporary for a const lvalue or rvalue reference, never for a reference-related
  (similar) operand (/5.4, CWG2352; Clang 23 differs, GCC 16 agrees) (#1396). Pointers whose pointee arrays or member pointers differ, and array and function
  sources, are diagnosed (#1399). Supported class-template bases and variable-template initializers
  replay dependent operands and check class completeness at the demand. Other combinations,
  including class `__is_nothrow_convertible`, get hard diagnostics.
  `__has_builtin` answers 0 while support is partial ([meta.rel], [dcl.init.ref], #218).
- `__reference_converts_from_temporary(T, U)` and `__reference_constructs_from_temporary(T, U)`
  tell whether a reference T initialized from VAL<U> binds to a temporary: a converted arithmetic
  value or a materialized prvalue, for arithmetic operands, void and reference-related classes
  (`tests/own/builtins/reference_temporary.run.cpp`). A supported class-template base or
  variable-template initializer replays either dependent operand. Class definitions are
  checked at the written expression for concrete queries and at demand for dependent
  queries. Unrelated classes that need user-defined conversions get a diagnostic.
  `__has_builtin` answers 0 while support is partial ([meta.unary.prop], [dcl.init.ref], #1064, #218).
- A written dependent `alignof(type-id)` in an integral class-template base argument is
  evaluated when the specialization requires its base. Forwarding aliases preserve the deferred
  argument. The result uses the target size type. An array of unknown bound is supported when
  its element type is complete. Pointer-only uses of the specialization leave evaluation deferred
  ([expr.alignof], [temp.inst], #1067).
- `__array_rank(T)` returns the number of array dimensions as the target size type. Unknown bounds
  count, incomplete element types need no layout, and nonarrays have rank zero. Dependent type
  operands resolve through template argument substitution and variable-template initializer replay.
  Namespace aliases replay rank arguments they discard and preserve conversions of forwarded rank
  defaults. An omitted type default checks those arguments before looking up the specialization
  ([meta.unary.prop.query], [temp.alias], [temp.arg], #1082).
- `__is_base_of(Base, Derived)` ignores top-level cv. Nonclass and union operands give false.
  The same nonunion class gives true, including when incomplete. For distinct nonunion classes,
  `Derived` must be complete. The trait follows the recorded base declarations, including private
  and ambiguous bases. A dependent occurrence in a supported class-template base or variable-template
  initializer checks completeness at instantiation. So does one in a function template body (also
  a member's, a member template's or a generic lambda's, in a template or not): the body defers it
  and its instantiation evaluates it, with completeness at the end of the unit
  (`tests/own/builtins/base_of_dependent_*`). In an atomic constraint (a concept's definition, a
  requires-clause or a nested-requirement) it is evaluated when the atom's satisfaction is
  checked, with completeness at that point (`tests/own/concepts/base-of-dependent*`).
  A concrete occurrence checks it at its original source point. Missing graph information gets a hard diagnostic when it prevents a result.
  `__has_builtin` answers 0 while support is partial ([meta.rel], [class.derived], #218).
- `__builtin_is_virtual_base_of(Base, Derived)`, `__is_pointer_interconvertible_base_of(Base,
  Derived)` and `__is_layout_compatible(T, U)` ([meta.rel]) are built-ins. The first walks the
  recorded base graph for a virtual base-specifier naming Base (`BaseRec::virtual_`, false until
  #310, so it answers false for every class ycxx accepts). The second is true for the same nonunion
  class or a base of a standard-layout Derived ([basic.compound]). The third compares the same type,
  enumerations' underlying types, and standard-layout classes' members (in order for structs, with
  effective alignment and bit-field widths; in any order for unions) ([class.mem.general]).
  Clang mode compares the strongest written member `alignas` for structs. A
  `[[no_unique_address]]` member is diagnosed as not supported yet: whether it ends the sequence
  depends on `__has_cpp_attribute`, which answers 0 for everything so far (#1141). The virtual-base
  trait requires a complete Derived when both operands are nonunion classes. Pointer
  interconvertibility permits the same incomplete class. Layout compatibility requires both
  class operands to be complete. Dependent operands
  are decided at instantiation in class-template bases, variable-template initializers, function
  template bodies and constraints (`tests/own/builtins/virtual-base-of-*`,
  `pointer-interconvertible-base-of-*`, `layout-compatible-*`, `relation-traits-*`). Unknown base graph,
  standard-layout or alignment facts (unapplied layout attributes) get hard diagnostics.
  `__has_builtin` answers 0 for all three (#218).
- `__is_empty(T)` gives false for nonclass types and unions. For a complete nonunion class, it
  checks polymorphism, non-static data members, and emptiness of the recorded bases. Missing base
  records and skipped layout attributes are diagnosed when they prevent a result. Dependent operands
  in supported class-template bases and variable-template initializers check completeness when
  instantiated. Concrete operands in those contexts check at the source point. A surviving use of a
  potentially constant variable-template specialization in an ordinary namespace-scope item or
  function body supplies the point for its initializer and nested initializer demands. Unevaluated
  uses with known types and discarded branches of a templated `if constexpr` leave that point to a
  later demand. A variable-template partial specialization with a declared type such as
  `Tag<empty<T>>` remains unsupported.
  `__has_builtin(__is_empty)` continues to answer 1 ([meta.unary.prop], #218).
- `__is_standard_layout(T)` checks completeness after removing array extents and accepts cv
  `void` as a false case. Supported class operands use the translation unit's member and base
  declarations to establish the property. It diagnoses unproved facts that affect the result.
  Dependent operands replay in supported class-template bases and variable-template initializers.
  `__has_builtin(__is_standard_layout)` answers 0 ([meta.unary.prop], [class.prop]).
  [#1030](https://github.com/yrashk/ycxx/issues/1030)
- `__is_final(T)` reads the completed definition for class types, including cv-qualified
  operands, unions, and class template specializations. Nonclass types give false. Direct
  class queries require completeness. Dependent class operands in supported class-template bases
  and variable-template initializers check completeness at instantiation ([meta.unary.prop]).
  [#1042](https://github.com/yrashk/ycxx/issues/1042) · [#1046](https://github.com/yrashk/ycxx/issues/1046)
- `__is_destructible(T)`, `__is_nothrow_destructible(T)`, and `__is_trivially_destructible(T)`
  give true for references, including references to incomplete types, supported non-enum scalars,
  and bounded arrays of those scalars. They give false for cv `void`, function types, and arrays
  of unknown bound. Incomplete class operands and bounded arrays of incomplete classes get a
  distinct diagnostic. Supported class-template bases and variable-template initializers replay
  dependent operands. The class completeness check uses the written point for concrete operands
  and the demand point for dependent operands. `__has_builtin` answers 0 for all three. Class
  destructibility, enum operands, and other dependent contexts remain in #218 ([meta.unary.prop]).
- Listed in docs/EXTENSIONS.md. Tests: `tests/own/builtins/`. `<cstdio>`: 53 → 33 errors.
- Not yet (#211 follow-up): the built-ins that need the object model, such as
  class constructibility, assignability and general convertibility, remaining `__is_trivially_*` and
  `__is_nothrow_*` predicates, and `__reference_binds_to_temporary`. Other dependent predicates,
  conversions of a dependent predicate to a non-`bool` parameter, and dependent array bounds
  remain in #209. Mangling dependent expressions is tracked in #773 and #780.

## Address-of builtin (#330)

`__builtin_addressof` implements the operation used by libycxx's `std::addressof`
([specialized.addressof]). It builds the ordinary address-of expression for a single lvalue
while bypassing overloaded `operator&`. The existing constant evaluator and code generator
consume the expression. Static member functions accessed through an object expression still
hit the ordinary address-of limitation. The tests in `tests/own/builtins/` cover overloaded
address operators, dependent wrappers, qualified operands, constant evaluation on a scalar
global, and a rejected rvalue.

## Bit-cast expressions (#1359)

`__builtin_bit_cast` reads a written destination type-id and preserves both operands through
template substitution. The evaluated subset covers initialized unsigned scalars and gap-free
flat records described by the target's size, byte order and member offsets. Result storage is
independent, including when the cast is discarded. Constant evaluation uses the existing
flat-record storage checks. Evaluated x87, padding and the remaining constant-evaluation
layouts stay in #330. The pinned x87 template branch parses when discarded.

## String length builtin (#531)

`__builtin_strlen` supports unqualified direct calls with one argument implicitly converted to
`const char*`. The result type matches `sizeof`. The interpreter and native targets share a byte
scan that evaluates the argument once and stops at the first zero. The interpreter keeps the
scan's reads indeterminately sequenced with the caller's other evaluations ([intro.execution])
and diagnoses reads past the end of an unterminated array in a required constant expression
([expr.const]). Native lowering requires no runtime `strlen` symbol. Fixtures in
`tests/own/builtins/` cover detection, result type, argument checks, and constant and runtime scans.

## Trailing-zero count builtins (#1381, #1479)

`__builtin_ctzg` accepts a single `unsigned int` and returns an `int` prvalue. It evaluates the
operand once. The initializer of a local `constexpr` with a value-dependent operand is evaluated
after function template substitution. The interpreter counts trailing zeros for nonzero inputs.
Native code uses BSF on x86-64 and RBIT followed by CLZ on AArch64. `__has_builtin` answers 1.
Other unsigned operand types and the optional fallback argument remain in #248.

`__builtin_ctz` uses the same lowering after implicitly converting its single argument to
`unsigned int`. The result type is `int` and `__has_builtin` answers 1. Spec and Clang
modes diagnose a reached zero operand in a required constant expression. GCC mode yields the
width of `unsigned int` during constant evaluation. At run time the GNU builtin's zero
operand has undefined behavior. The other fixed-width spellings remain in #248.

## Leading-zero count builtin (#1539)

`__builtin_clzll` implicitly converts one argument to `unsigned long long`, evaluates it once
and counts its leading zero bits as an `int`. Constant evaluation uses the target operand width.
Spec and Clang modes diagnose a reached zero operand in a required constant expression. GCC
mode returns the operand width during constant evaluation. Runtime zero has undefined behavior.
Native lowering uses BSR on x86-64 and CLZ on AArch64. `__has_builtin` answers 1.

## C library string and memory builtins (#248, #1093, #1170)

`__builtin_memchr`, `__builtin_memcmp`, `__builtin_memcpy`, `__builtin_memmove`,
`__builtin_memset`, `__builtin_strchr`, `__builtin_strpbrk`, `__builtin_strrchr` and
`__builtin_strstr` are direct calls of the C library's function of the same name, as libycxx's
`<cstring>`, its run-time string helpers (`include/ycxx/core/mem_builtins.hpp`) and the run-time
paths of its containers use them. Each is one `lex::BuiltinFunction::library` entry, and its C
signature is a row of `sema::string_functions`; the searches return `char*`, as Clang's and GCC's
do. Sema converts the arguments to the C signature and calls an implicit `extern "C"` declaration
with the target's `<string.h>` exception specification (`World::string_function`), so a written
declaration or definition that matches the target's header is the same entity. One with another
exception specification is a second entity (whether it should be is #1114), but in both object
writers an entity the object does not define shares the symbol of its name: the definition of
that name if the object has one, else one undefined symbol (#1095). The object still lists the
function once (`unit.string_builtin_symbols`, ELF and Mach-O), and each definition keeps its own
symbol (#694). The unit then references the function as an
undefined symbol, which libSystem or the C library provides at link time. A dependent argument
defers the call to instantiation. Fixtures in `tests/own/builtins/` (`memchr-memcmp-*`,
`string-memory-*`) cover detection, signatures, argument checks and run-time results.

`__builtin_strcmp` uses the same path with the signature `int strcmp(const char*, const char*)`.
The runtime comparison in the pinned libycxx's `type_info::operator==` and `type_info::before`
uses the C library's unsigned-character ordering for null-terminated strings.
[[cstring.syn]](https://eel.is/c++draft/cstring.syn) · [#1170](https://github.com/yrashk/ycxx/issues/1170)

`memchr` and `memcmp` are constant expressions (#1092); the others are not. Their rows carry a
`constant` (`ast::LibraryBuiltin`), which marks the call (`ast::Call::builtin`), and IR generation
branches on `consteval_br`: the runtime edge is the library call, which `lower` keeps, and the
consteval edge is the `builtin` op (`ir::BuiltinFn`), which the interpreter evaluates. It reads
objects of narrow character type, and elements of arrays of them, as `unsigned char` with the
interpreter's access, lifetime and initialization checks (`Machine::bytes_at`, `read_byte`); any
other object is `Reason::byte_type`. In spec mode, `memchr` reads in order up to the match and
`memcmp` checks that both objects hold all `n` bytes before reading up to the first difference.
GCC mode reads the full range for both, and Clang mode checks bytes as read. The `memchr-*`
fixtures mark the oracle disagreements, and the compatibility test exercises the selected modes.

## Pack expansions in builtin arguments (#1099)

An argument of a builtin with ordinary expression arguments (`src/lex/builtins.cppm`'s
`builtin_functions`) may be a pack expansion, as a call's may (`__builtin_strlen(a...)`,
`__builtin_memcmp(a..., n)`). The parser reads it through the same `expandable` path as call
arguments. While a pack is unexpanded the call is one dependent operand. Instantiation checks
arity and conversions on the expanded arguments. In GCC mode, `__builtin_addressof` requires a
single written operand and diagnoses an instantiated expansion. Fixtures:
`tests/own/builtins/pack-arguments*`.

## GNU constantness probe (#1065)

`__builtin_constant_p(e)` returns an `int` prvalue. It gives 1 when a supported scalar or
string operand folds safely to a known value, including a known false expression. It gives
0 when the proof is unavailable. Operand formation and exception checks use the written
expression. Native execution and constant evaluation of the result leave the operand
unevaluated. A dependent probe is resolved when its template body is instantiated.
Calls to user functions and values from the active constexpr frame remain outside the
proof domain. `__has_builtin` answers 0 while support is partial.
[Issue #1065](https://github.com/yrashk/ycxx/issues/1065)

## Checked integer builtins (#1077, #1636)

The direct GNU `__builtin_add_overflow` and `__builtin_mul_overflow` calls accept matching
signed target-`intmax_t` operands with a writable destination of the same type during
constant evaluation. Constant evaluation and native code also accept differing standard
unsigned integer operand and destination types up to 64 bits. The call evaluates each argument
once, stores the converted result once and returns whether it differs from the exact arithmetic
result. The destination uses the usual object and lifetime rules. `__has_builtin` answers 0 for
the partial domain. [#1636](https://github.com/yrashk/ycxx/issues/1636)

Native emission can omit an unreferenced immediate function template specialization. A named
namespace-scope specialization with checked arithmetic that is retained by the reference
walk keeps its symbol and entry ABI checks with an unreachable native body. Ordinary external
definitions of that same specialization coalesce across translation units. Constant evaluation
uses the original body.
[#1077](https://github.com/yrashk/ycxx/issues/1077)

## Trap builtin (#548)

`__builtin_trap()` is a zero-argument direct call of type `void`. It ends the
executing IR block and requires no external runtime symbol. Linux x86-64 emits UD2
and Darwin arm64 emits BRK. The builtin registry also exposes it through
`__has_builtin`.

`__builtin_unreachable()` ([utility.unreachable], #330) is the same `void` call and lowers to
the same IR `unreachable`, flagged `assumed_unreachable`. Native code traps as above. A
constant evaluation that reaches it fails with "execution reached __builtin_unreachable()"
([expr.const]). IR node flags are 16 bits wide to make room for the flag.

## Allocation builtins (#1681)

`__builtin_operator_new(args)` and `__builtin_operator_delete(args)` are lowered in sema to the
call `::operator new(args)` or `::operator delete(args)` would be: the global overload set (the
implicit usual declarations plus written ones) and ordinary overload resolution. The selected
function must be a replaceable one (`World::replaceable_allocation_function`: the usual forms and
their `std::nothrow_t` forms); anything else is diagnosed. Dependent calls are checked when the
instantiation is reparsed. The result is an ordinary `ast::Call`, so native code calls the
symbol and a program's replacement is used. The calls are not elided or merged. Constant
evaluation of them waits for constexpr dynamic allocation (#321).

## Function spelling for type ordering (#1361)

`__PRETTY_FUNCTION__` in a namespace-scope function body is a static `const char[N]` lvalue.
Function template definitions defer the text and array bound until the selected body is
replayed. The NUL-terminated text uses canonical argument types and qualified names, so
aliases of a type have the same spelling. It distinguishes nested types, qualifications,
indirections, and class template arguments in the supported domain. The spelling uses the
declared signature, including any return placeholder. Constant evaluation and native code
use the same storage path as a string literal.

Spelling is supported for the standard fundamental types, 128-bit integers, named classes
and enums with named scopes, and their array, function, pointer and reference forms. Argument
lists can include packs, named templates and integral, floating or null pointer values.
Other arguments diagnose an unsupported spelling when the body is instantiated.
[#1361](https://github.com/yrashk/ycxx/issues/1361)

## Source location builtins (#327)

`__builtin_LINE()` is a zero-argument constant expression. Its type is `unsigned int` in spec
and Clang modes and `int` in GCC mode. It uses the presumed source line, including through
macro expansion and `#line`. An omitted ordinary function
argument evaluates its default with the invocation line, which nested defaults inherit.
The interpreter and native code share the same scoped evaluation. Translation units with the
builtin track presumed lines in their semantic input.
[Design](docs/design/source-line-defaults.md)

x86-64 Linux and arm64 Darwin support the pinned libycxx `std::source_location::current()` descriptor
for ordinary omitted arguments. The descriptor stores the presumed filename, line,
one-based byte column and enclosing function in translation-unit-local storage.
Resident results observe edits to presumed filenames and columns as well as lines.
Constructor and default member initializer anchoring, and broader immediate-default
contexts, remain in [#327](https://github.com/yrashk/ycxx/issues/327).

## Basic Linux asm statements (#950)

Statement-position `asm("int3");` and GNU `asm volatile("int3");` emit one INT3
instruction on Linux x86-64. Execution may continue after the trap is handled.
The parser accepts ordinary decoded strings in discarded `if constexpr` arms.
Native lowering diagnoses a selected instruction outside the profile at its asm
statement. Constant evaluation rejects an asm it executes. Other instructions,
operands, and targets are tracked in [#190](https://github.com/yrashk/ycxx/issues/190)
and [#659](https://github.com/yrashk/ycxx/issues/659).
[Design](docs/design/linux-basic-asm.md) · [#950](https://github.com/yrashk/ycxx/issues/950)

## Templates, ζ step 6: concepts and `if constexpr` (#88, 2026-11-18)

Fable decided the representation (ast.md §6.6); the Opus session wrote the code.
- **Concept definitions** ([temp.concept]) are `concept_` entities whose `DeclSema.constraints` is
  the normal form of their constraint-expression. A concept-id `C<args>` is a `bool` constant, or
  value-dependent while an argument is.
- **Associated constraints** of function templates ([temp.constr.decl]): type-constraints
  (`template<C T>`, `template<C<U> T>`), the head's requires-clause and the trailing one, in
  normal form ([temp.constr.normal]): `VConstraint` conjunctions and disjunctions over `VAtom`s,
  each the atom's tokens and a mapping of the parameters it names.
- **Satisfaction** ([temp.constr.sat]): an atom is re-analysed from its tokens with its mapping
  bound, its function parameters declared and argument-dependent lookup at the point of the
  check; an error there makes it unsatisfied, a non-`bool` or non-constant result and "not
  supported" are hard errors (`survives_substitution`), so a missing feature never picks another
  overload. Results are memoized per atom (`Program::atom_satisfied`). Where the constraint is
  written, a type trait with dependent operands is left for satisfaction
  (`Sema::trait_at_satisfaction`), which judges the classes it needs complete at the point of the
  check (`AtItem::satisfying_atom`). A `bool` type-trait built-in can be a requires-clause's
  operand without parentheses (`tests/own/concepts/requires-clause-trait-primary.run.cpp`).
- **Overload resolution** drops candidates whose constraints are not satisfied
  ([over.match.viable]/3) and, between templates that are otherwise equally specialized, prefers
  the one whose constraints subsume the other's ([temp.constr.order], [temp.func.order]/6).
  Declarations whose clauses differ are distinct overloads; equivalent clauses (as tokens, with
  parameters by position) redeclare.
- **Requires-expressions** ([expr.prim.req]) with parameters and simple, type, compound (with a
  return-type-requirement, also a qualified concept) and nested requirements, checked in place:
  false in a templated entity when a requirement is invalid, ill-formed elsewhere.
  During substitution with bound template arguments, a decisive failure ends the
  requirement sequence before later dependent checks. A separately instantiated class
  still emits a hard error when a reached requirement needs it (#1283).
- **`if constexpr`** ([stmt.if]/2): the condition is evaluated as it is parsed; an
  instantiation skips the discarded branch, and outside templates its odr-uses demand nothing.
- **`static_assert` in a template definition** (#241, P2593, [dcl.pre]): a non-dependent condition
  must be a constant expression. A false result is diagnosed when the assertion is instantiated.
- Also: the stand-in `Program` indexes declared names, so a namespace-scope lookup visits only
  its own name's items (draft tests including libycxx headers: 8 s → 5 s); floating literals
  report "not supported" instead of "expected '<'".
- Tests: `tests/own/templates/c6-*` (18, agreeing with Clang and GCC); 6 more draft examples
  pass. `<cstdio>`: 33 → 18 errors.
- Ordinary function overloads with explicit trailing type packs preserve the pack in atomic
  constraint substitution. Empty packs, order, and reference types are retained (#234).
- A directly named, non-dependent specialization of a primary class template with explicit type
  or value arguments checks its constraints even when the use does not need a complete class
  (#234, [#908](https://github.com/yrashk/ycxx/issues/908)).
- In non-template code outside requires-expression bodies, directly naming a primary namespace
  variable-template specialization with explicit non-dependent type arguments checks its associated
  constraints. This includes unevaluated uses such as `decltype`. A type-constraint or a leading
  requires-clause contributes to that check. [#234](https://github.com/yrashk/ycxx/issues/234)
- A compound requirement `{ e } noexcept;` holds when the operator `noexcept(e)` gives true: a
  call to a potentially-throwing function or a temporary with a throwing destructor fails it
  ([expr.prim.req.compound]/2, #775, `tests/own/concepts/compound-noexcept.run.cpp`).
- Partial specializations of class and variable templates are matched and ordered by their
  constraints ([temp.spec.partial.match]/2, [temp.spec.partial.order]). One whose arguments are the
  primary's own is allowed when it is more constrained, and two with the same arguments are
  ordered by subsumption or are ambiguous at use. A constrained partial's pattern is keyed by its
  constraints' token fingerprint too (Fable's decision on #234). Tests:
  `tests/own/concepts/constrained-partial*`. Its explicit self-reference, out-of-class members and
  member templates are #832, and head identity for all partials is #833.
- Class template heads and class partial specializations accept parenthesized constraint
  operands joined by `&&` or `||` ([temp.pre], #839).
- Not yet: class and variable template constraints outside those cases, members, broader
  constrained packs, fold-expanded constraints, constrained placeholders
  outside ordinary block declarations (#234). Open follow-ups include re-verification of satisfaction at END
  (#235), Itanium constraint mangling (#236), recorded discarded extents for `if constexpr` (#237),
  and value-dependent mapping arguments (#209).

## Trailing requires-clauses on members of class templates (#1417)

- A member function, constructor or prospective destructor of a class template, a nested class of
  one, or a member template can have a trailing requires-clause ([class.mem.general],
  [dcl.decl.general]/4), defaulted (`B() requires C<T> = default;`), deleted or user-provided, and
  so can its out-of-class definition, which must repeat it ([temp.over.link]/6). A non-templated
  function with one is an error.
- The clause is part of the member's identity (`EntityRec::constraints`, its own tokens, mixed for
  non-template functions only when present, [basic.scope.scope]/4), so members that differ only in
  it are overloads. Each member-declarator has its own (GCC agrees; Clang gives a later declarator
  the earlier clause, docs/compatibility.toml). An explicit specialization names the member its
  specialization satisfies. The clause is mangled after the parameters (`Q`), as Clang and GCC
  do; a concept-id clause matches them exactly, other expressions use the vendor form templates use
  too ([#236](https://github.com/yrashk/ycxx/issues/236)). A specialization reads it as the pattern does and substitutes its arguments into
  the atoms' mappings ([temp.constr.normal]); nothing in it is checked before the member is a
  candidate ([temp.inst]), and a mapping that cannot be formed is unsatisfied
  ([temp.constr.atomic]/3). `MemberDeclRec::constraints` keeps the specialization's normal form.
- Member calls, constructor selection, qualified calls and addresses of static members skip a
  candidate whose constraints are not satisfied ([over.match.viable]/3, [over.over]/5) and prefer
  the more constrained of non-template functions with the same parameters
  ([over.match.best.general]/2.6). `auto` deduces from such a set when its satisfied members agree
  ([temp.deduct.call]/6.2).
- When a specialization is complete, `Program::eligible_special_members` marks the special members
  that are not eligible: unsatisfied, or outranked by a more constrained satisfied one of their kind
  and parameters ([special]/6). They stay user-declared and suppress implicit declarations, but are
  no candidates, are never defined and do not count for the class's triviality or POD layout. Of the
  prospective destructors one is selected ([class.dtor]/4); none or several is an error.
- Not yet: constrained friends are
  [#1180](https://github.com/yrashk/ycxx/issues/1180); type-trait built-ins on class template
  specializations are [#218](https://github.com/yrashk/ycxx/issues/218); a qualified call of an
  overloaded static member also instantiates the overloads it did not select
  ([#1461](https://github.com/yrashk/ycxx/issues/1461)); a non-static member in `sizeof` in a clause
  ([#1299](https://github.com/yrashk/ycxx/issues/1299)); the injected-class-name naming a dependent
  base's member in a clause ([#1524](https://github.com/yrashk/ycxx/issues/1524)); out-of-class
  constructors of a class template with a requires-clause in its head are never emitted
  ([#1530](https://github.com/yrashk/ycxx/issues/1530)).
- Tests: `tests/own/member-requires/` (52: a spec-only author's, the review's and regressions;
  XFAILs cite #218, #157, #1150, #1299, #1461, #1524 and #1530).

## Access control and friends (#119, 2026-11-18)

- **Records:** `ClassRec::access` holds each member's access by entity: data members, member
  functions, constructors, static data members, and nested enumerations and type aliases
  (`NestedType::entity`). The default is private for `class` and public for `struct`
  ([class.access.spec]). `ClassRec::friends` holds the friends' entities.
- **Friend declarations** (`friend_declaration` in parse.cppm, `decl_flag::friend_`) are not
  members, and declare no name for ordinary lookup ([class.friend]/7). Three kinds:
  - `friend class X;`: the class lookup finds, else the namespace-scope class the declaration
    introduces;
  - `friend R f(…);`: the function of the enclosing namespace with that name and type, the same
    entity its namespace-scope declarations name;
  - `friend R C::g(…);`: a member function of another complete class.
- An ordinary unqualified friend definition in a non-template namespace-scope class defines a
  namespace function, implicitly inline in the global module. Its body sees the completed
  class, including later members and types, with no implicit object parameter. A declaration in
  another class can grant the same entity access. Linux uses the existing weak-ELF path for
  supported external inline functions. [#259](https://github.com/yrashk/ycxx/issues/259) ·
  [#422](https://github.com/yrashk/ycxx/issues/422)
- Ordinary binary hidden friend operators participate in ADL and compete with nonmember
  overloads from the associated namespaces. Qualified ordinary lookup still requires a
  namespace declaration. The existing Linux inline gate checks both object orders.
- The unary operators `+ - ! ~ * &`, prefix `++` and `--`, and the compound assignments find
  hidden friends by ADL, on class and enumeration operands and, for a dependent operand, at the
  point of instantiation (#1178). With no class operand only a candidate with an enumeration
  parameter matching its operand takes part ([over.match.oper]/3). Unary operators rank the
  conversions of hidden friends, other nonmembers and members together, a non-template before a
  template. Compound assignments do
  so where `ordinary_binary_choice` models the conversions; elsewhere the member is tried first
  (#219). A postfix friend can be declared, and called in function notation only (#219). Access
  and deletion are checked for the selected operator. Overloaded compound assignment is not yet
  sequenced right to left (#1275). A friend function of a nested class is not supported (#1277).
- **Friends in class templates** ([temp.friend], `templated_friend` in parse.cppm, #259): an
  unqualified friend function definition in a namespace-scope class template (or partial
  specialization) declares, for each specialization, an inline function of the namespace with
  the arguments substituted. It joins the specialization's `ClassRec::adl_friends`, and
  `member_patterns_` maps it to its item and specialization, so its body is instantiated with
  the class's arguments when odr-used ([temp.inst]/4), in the specialization's complete-class
  scope. `Questions::friend_decl` gives ADL the specialization's declaration. A dependent
  noexcept-specifier is kept as tokens and replayed in that scope ([except.spec]/13). A
  class template specialization is now an associated class of ADL, instantiated where needed,
  as Clang and GCC do. Two specializations defining one function report a redefinition, as
  does a namespace definition of it (within an item group; #1148 across groups). The friend
  is coalesced as the class's members are, and binds locally when its signature names a class
  local to the unit (a closure type included).
- **Friend function templates** defined in a class or class template
  (`template <class T> friend R f(…) {…}`) are function templates of the namespace that only ADL
  finds; a class template's specialization binds its own arguments for theirs (#259).
- **Friend class templates** (`template <class U> friend class X;`, `friend_class_template` in
  parse.cppm, [temp.friend]/1, #259), in a class or a class template: `ClassRec::friends` holds
  the class template's entity, and `ClassRec::befriends` grants access to each of its
  specializations, from the primary or a partial specialization. Unqualified lookup stops at
  the innermost enclosing namespace, including using-declarations and using-directives that
  contribute at that level ([dcl.type.elab]/4, [namespace.udir]). Finding no declaration names a
  template of that namespace (keyed by its head at depth 0), visible after a namespace declaration.
  `N::X` must name an earlier class template. Matching heads compare parameter kinds, packs and
  non-type parameter types, with the corresponding template parameter positions paired (#1211).
  A friend head cannot give default arguments or redeclare a template parameter. A head's
  constraints are not compared with the template's yet (#1182), and a later conflicting
  declaration of a friend-declared name is not diagnosed (#1184).
- **Class friends in class templates** (`friend class X;`, `friend N::X;`, `friend struct N::X<T>;`,
  `friend C<T>;`) are analysed again in each specialization with its arguments; a dependent one
  befriends nothing in the pattern. `friend class X;` still looks past the innermost namespace,
  and `friend typename T::F;` and `friend class T::G;` are rejected (#1185).
- **Checks** (`Sema::accessible`, `check_access`): a private or protected member may be named in
  a member function or default member initializer of its class, in the class's scope (the
  initializer of a static data member defined outside it), or in a friend function or friend
  class (`Sema::current_function`). It is checked when named through `.` and `->`, by qualified
  and unqualified names, as a static member, enumerator or nested type, for the member function
  overload resolution chooses ([class.access]/4), and for the constructor an initialization
  chooses. Protected is checked as private until base classes exist.
- C-linkage friend definitions, defaults on friend declarations, non-defining friend functions in
  class templates, private member operator wording, and elaborated types in member parameters
  remain under [#259](https://github.com/yrashk/ycxx/issues/259). Non-defining
  friend function templates are [#1179](https://github.com/yrashk/ycxx/issues/1179), friends with a
  trailing requires-clause in class templates [#1180](https://github.com/yrashk/ycxx/issues/1180)
  and deleted friends of class templates and friend function templates [#1181](https://github.com/yrashk/ycxx/issues/1181).
  Both targets coalesce these inline friend definitions across objects (#422, [#124](https://github.com/yrashk/ycxx/issues/124)).
- Both ELF and Mach-O coalesce eligible nonthrowing, nonvirtual in-class destructor definitions
  and implicitly defined destructors, including those of class-template specializations. A
  specialization naming a translation-unit-local type retains local destructor binding, including
  when an enum value of that type is the template argument. An explicitly specialized destructor
  coalesces when its own declaration is inline and meets the same eligibility checks. The
  separate-object tests check both orders, distinct specializations, and the duplicate diagnostic
  for ordinary and explicitly specialized non-inline definitions.
  [#1025](https://github.com/yrashk/ycxx/issues/1025) · [#1040](https://github.com/yrashk/ycxx/issues/1040)
- A namespace-scope declaration's qualified types are checked once what it declares is known
  (`Sema::defer_type_access`, `settle_type_access`): a member's definition outside its class and
  a friend's declaration may name the class's private types ([class.access]/6–7). A friend
  qualified by a namespace names an earlier declaration ([namespace.memdef]/3).
- Tests: `tests/own/classes/s6-access-*` (five reject tests, two run tests); draft examples
  class.friend-1 and class.name-4 pass.

## Dependent type friends in class templates (#689)

A class template can name a non-pack type parameter in a standalone `friend Authority;`
declaration. Instantiation grants access to the class of that specialization's argument,
including a cv-qualified or incomplete class, or ignores a non-class argument
([class.friend], [temp.friend]). The existing access checks use each specialization's own friend
list. The declaration introduces no name into lookup.
A type inherited from a nondependent base takes precedence over the template parameter
([temp.local]).
[#689](https://github.com/yrashk/ycxx/issues/689)

## Class scope before the template parameter scope (#800)

Unqualified lookup in a class template, and in a definition of its member outside it, searches
the class's scope with its nondependent bases before the template parameter scope of the class
template or of that definition ([temp.local]/7). A member of a nondependent base, a base's
injected-class-name, or in a definition outside the class a member of the class itself, hides a
template parameter of the same name: in expressions, type names, nested-name-specifiers and
template names (`Sema::template_param`). A member template's own parameters are still found
first; a dependent base is not searched. Redeclaring a hidden parameter's name is still an error
([temp.local]/6, `Sema::declared_template_param`).
- Tests: `tests/own/temp-local/` (one run test, two reject tests).
- The same rule applies through enclosing classes of an in-class member-class definition (#1187).

## The injected-class-name as a template-name (#1567, #1177)

In a class template, a partial specialization or a specialization, the class's own name followed
by `<` names the template, so `A<U>` is the specialization `A<U>` ([temp.local]/1). This covers
constructor and member function parameters, return types, and qualifiers such as `A<U>::member`
and `typename A<U>::type`, in the class and in out-of-class member definitions. This is the
converting-constructor pattern of `allocator`, `pair` and `optional`. The class's constructors
share its name but are not found by lookup ([class.ctor]/1). They no longer hide it
(`Sema::injected_class_template`). Plain `A` is still the current specialization.
- Tests: `tests/own/temp-local/injected-template-name.run.cpp`, `injected-template-name-args.reject.cpp`.
- Open: a non-dependent other specialization (`A<long>` by value, `A<long>::f()`) in the class's
  own member bodies is still reported incomplete (#1149). A member class template's injected
  name waits for member class templates (#340). A base's injected-class-name without arguments
  (`struct D : B<int> { B b; }`) is #1590.

## Templates, ζ step 7: static data members of class templates (#88, 2026-11-18)

- A class template's static data members are declared with its other members. A specialization
  declares them; the in-class initializer of one defined in the class (constexpr or inline) is
  instantiated only when the member is used ([temp.inst]/3, `Program::member_value`), from the
  extent the template's analysis recorded, under the nesting limit ([temp.inst]/16). Only the
  members used are defined in the unit (`Program::used_members`); as with variable templates,
  the definitions are not yet coalesced across units (#124).
- In the template, a static member whose initializer is value-dependent is value-dependent
  ([temp.dep.constexpr]/2.3); one whose initializer is not is a constant there. `static const T k
  = 5;` is checked again in each specialization.
- A static constexpr member initializer can read fields of a flat aggregate temporary with
  scalar default member initializers. Concrete specialization replay uses the same ordered field
  reads as an ordinary required constant initializer ([dcl.init.aggr], [temp.inst], [expr.const]).
  [#991](https://github.com/yrashk/ycxx/issues/991)
- `X<args>::m` with a dependent qualifier names a value, not a type, in a statement, a condition, a
  cast's parentheses or a template argument, unless `typename` says otherwise
  ([temp.res.general]/4).
- An explicitly specialized static data member, `template<> int A<char>::x = 0;`, defines it; one
  that already has an in-class initializer cannot be given another.
- A member whose initializer is in error is declared, and its uses report nothing more.
- Tests: `tests/own/templates/s7-*` (11, agreeing with Clang and GCC),
  `tests/incremental/templates/static-members.incr`; 1 more draft example passes.
  `<cstdio>`: 18 → 16 errors.
- Not yet: a definition outside the class, `template<class T> int X<T>::n = 5;`, reported as not
  supported (#256); an array bound or template argument that is a dependent expression other than
  a parameter (`int a[sizeof(T)]`, `F<N - 1>`, #209); dynamic initialization of an inline static member. Found on the way: #262, #263, #264.

## `__GNUC__` and system-header warnings (#229, 2026-11-19)

- ycxx predefines `__GNUC__` 4.2.1, as Clang does (`:predefined`; DECISIONS §1.5, revising #112),
  so the SDK's `sys/cdefs.h` no longer warns "Unsupported compiler detected". Not `__GNUG__`,
  `__clang__` or `__llvm__`. libycxx tests `__ycxx__` first (yrashk/libycxx#28).
- What the SDK's `__GNUC__` paths then use, in the headers libycxx's C headers reach, is in
  docs/EXTENSIONS.md. `__builtin_va_list` names the target's va_list type (`char*` on Darwin)
  where lookup finds nothing else (`ast::Target::va_list`, `Sema::builtin_type_name`).
  The `__builtin_*` functions ycxx lacks (such as `__builtin_alloca`) are diagnosed as
  "built-in function '…' is not supported yet", their arguments skipped (`lex::lacking_builtins`,
  `Sema::unsupported_builtin`). Any other `__builtin_` name is an ordinary one, so a
  requires-expression probing it is unsatisfied.
- Warnings located in a system header are not reported; errors are, and so is `#warning`, as
  with Clang and GCC. A system header is one found in a system directory (a `-I` directory that
  is also an `-isystem` one is dropped from the `-I` list), or one a system header includes
  (`Config::system_header`, `Frame::system`, part of a region's cache key). Phase 4's warnings
  are dropped when the region finishes; phases 5 and 6's when flattened (`Flat::system`), by
  their token's outermost expansion site, and one spelled in a system header's macro is reported
  where the macro is used. Sema has no warnings yet; when it gets some, it needs the region's
  system files too.
  - Not yet: `Flat::system` is by file, so a header without a guard included both as a system
    header and not has phases 5 and 6's warnings hidden in both.
- The test driver checks warnings where a test has `//! warning` markers (docs/TESTING.md), and
  reads markers in headers a test includes with `<...>` from its `FLAGS` directories.
- Tests: `tests/own/pp/predefined-gnuc.pp.cpp`, `tests/own/headers/system-warnings.accept.cpp`,
  `tests/own/headers/stdio-no-warnings.accept.cpp` (`<stdio.h>` with no warning on Darwin),
  `tests/own/headers/system-error.reject.cpp`,
  `tests/own/builtins/va-list.run.cpp`, `tests/own/builtins/unsupported.reject.cpp`, `tests/own/builtins/unknown-probe.accept.cpp`. `<cstdio>`: 25 errors before and after, and no warning
  (was 1). `<cmath>` and `<cstdlib>` gain errors in the SDK's `__GNUC__` inline functions (local
  unions, floating-point operations, `__builtin_fabs*`), which already had thousands.

## Warning controls for freestanding builds

- The driver and editor checks accept `-Wall`, `-Wextra` and their negative forms. The
  multicharacter-literal warning is on by default. It responds to `-Wno-all` and `-Wall` in
  spec and Clang modes. GCC mode leaves this warning outside `-Wall`. The individual `-Wmultichar`
  and `-Wno-multichar` controls take precedence over the group. The last equally specific
  occurrence wins.
- `-Wextra` selects sized-deallocation warnings as in GCC 16, subject to
  `-Wsized-deallocation` and `-Wno-sized-deallocation`. `-Wattributes` and `-Wno-attributes`
  select attribute warnings separately. Neither category has a diagnostic producer yet.
- The selection happens when a request renders its diagnostics, including editor requests
  configured from `compile_commands.json`. Required `#warning` diagnostics, the system-header
  warning rules and errors are unaffected. Direct invocations still reject unsupported
  warning controls. [#1014](https://github.com/yrashk/ycxx/issues/1014)

## GNU pointer qualifiers in glibc declarations (#384)

- The parser accepts `__restrict` and `__restrict__` after `*` in pointer declarators
  ([dcl.ptr]), including the parameter declarations in glibc's `<stdio.h>`. Type construction
  and alias analysis use the ordinary pointer rules. `unit.sema` covers both spellings and
  invalid declarators. See `docs/EXTENSIONS.md`.

## Linux compiler resource `<stddef.h>` (#133)

- For x86-64 Linux, `resources/linux/include` is searched after libycxx, so its `<stddef.h>`
  forwards glibc's `__need_size_t`, `__need_wchar_t` and `__need_NULL` requests to ycxx. Each
  request is consumed, and partial inclusions can precede libycxx's full `<cstddef>` inclusion.
  The full resource header supplies `max_align_t` with 16-byte alignment (AMD64 psABI §3.1.2).
  `unit.pp` exercises both paths through the configured header search.

## Compiler resource `<stdarg.h>` (#383, #676)

- Linux `__builtin_va_list` is an array of one compiler-owned record with the two unsigned
  offsets and two pointers of the AMD64 psABI §3.5.7. Ordinary array and record semantics give it
  size 24 and alignment 8 and adjust it to a pointer as a function parameter ([dcl.array],
  [dcl.fct]). The builtin name also works with explicit global qualification.
- `resources/common/include` holds the resource headers every target shares. It is searched
  after libycxx and Linux's own resource headers, and before glibc or the macOS SDK, neither of
  which ships `<stdarg.h>`. The SDK's `<stdio.h>` and `<wchar.h>` include it.
- The resource `<stdarg.h>` consumes each `__need___va_list` request and supplies the guarded
  `__gnuc_va_list` typedef. A full include also supplies `va_list` and the `va_start`,
  `va_arg`, `va_copy`, `va_end` and `__va_copy` macros ([cstdarg.syn]). Their replacement
  lists match libycxx's `<cstdarg>`, so the two headers combine in either order.
  `own/variadic/stdarg-header` runs a variadic definition through them on both targets. glibc's
  `<stdio.h>` preprocesses, and `unit.linux_stdarg` checks that path, both inclusion orders,
  and type semantics. The pinned libycxx `<cstdarg>` exposes the Linux variadic operations
  described in the x86-64 backend status (#196).

## Linux resource `<float.h>` and `<limits.h>` (#475)

- Direct `<float.h>` shares the target's floating-point macros with the pinned libycxx
  `<cfloat>` ([cfloat.syn]). The resource uses the macro-only provider for C linkage.
- Linux `<limits.h>` supplies the compiler's target-dependent integer limits and widths
  from libycxx's macro-only `<climits>` provider before entering glibc's header. The resource scopes glibc's
  continuation marker to that inclusion and lets glibc supply the locale bound and POSIX
  limits. Other libc providers keep their normal `<limits.h>`, and the resource can
  supply compiler limits when a sysroot has no provider ([climits.syn]).

## glibc `<stdint.h>` (#657)

- In spec and GCC modes, the integer-constant macros use glibc's spaced spelling, such as
  `c ## L`, so the redefinitions in `<stdint.h>` are identical ([cpp.replace.general]/2).
  Clang mode uses the compact spelling and permits the redefinitions in system headers.
  Tests cover both targets in `own/pp/predefined-int-c-redefine` and Linux in `own/headers/glibc-stdint`.
- The pinned libycxx lets the target's `<stdint.h>` supply integer limits before libycxx supplies
  fallback macros. `<cstdint>` uses compiler predefines when a target C header is unavailable or
  the translation unit is freestanding. Linux integration tests cover `<cstdint>` before
  `<cinttypes>` and `<inttypes.h>` before `<cstdint>`.
  [#664](https://github.com/yrashk/ycxx/issues/664) · [libycxx#50](https://github.com/yrashk/libycxx/pull/50)

## 128-bit integers (#1172)

- `__int128` and `unsigned __int128` are `ast::Builtin::int128` and `unsigned_int128`, after
  `unsigned_long_long`, so `ast::is_integral` covers them. `ast::is_extended_signed` and
  `is_extended_unsigned` name them, and `is_signed_integer` and `is_unsigned_integer` take
  standard and extended types together. Both targets lay them out in 16 bytes with 16-byte
  alignment; their rank is 7, above `long long`'s 6.
- `__int128` is a keyword (`Tk::kw___int128`, the last one). The decl-specifier rules accept it
  alone or with `signed` or `unsigned`, in either order. `__int128_t` and `__uint128_t` are
  typedef-names the implementation declares, beside `__builtin_va_list`.
- `__SIZEOF_INT128__` is 16 on both targets, from `ast::Target`, checked against Clang in
  `own/pp/predefined-target`.
- Symbols spell the types `n` and `o`. Template arguments print their values with
  `ast::wide::decimal`, so a value beyond 64 bits is written in full.
- `ast::wide` holds 128-bit arithmetic over `ast::Bits128`: add, sub, mul, shifts, comparisons
  and division by shift and subtract. The interpreter's `Value` and `Result` carry the high half
  in `hi`, which is 0 for every narrower type. `arith`, `divide`, `shift`, `icmp`, `iconv`,
  the floating conversions, `tobool`, `switch` and pointer offsets take 128-bit operands.
  Conversion to a floating type rounds the top 64 bits with a sticky bit.
- List-initialization narrowing reads constants in 128 bits. `sema::ValueRec::wide` marks an
  integer outside [-2^63, 2^64), so concrete array bounds, bit-field widths and array new
  bounds reject it. Substitution checks a dependent array bound against the target's
  `size_t` range before conversion. [#1212](https://github.com/yrashk/ycxx/issues/1212)
- Static data of 16-byte integer type is written with its high half.
- **Native code**: L2 has `i128` (`ycxx.ir.l2`), and `lower` writes `i128` operations for every
  target. `legalize` (`ycxx.ir.legalize`) is an L2-to-L2 pass, run at the end of the `lower` cell,
  that expands what `Int128Support` does not list for the target: into two i64 halves, with carries
  and the new `umulh`, or into a call of a compiler runtime function (`ir::Runtime`; `Lowered::runtime`
  carries the symbols to the object). The table is empty for AArch64 and x86-64. `unit.lower` runs
  it for a mock target with native 128-bit `add`, `load` and `store`. Both code generators refuse a
  body with an `i128` left in it.
- **Calls**: `lower` splits a 128-bit parameter, argument or result into two integer words
  (`LowerConfig::int128_in_words`). Both go in registers when two are left, else on the stack at
  16 bytes. Apple's arm64 convention has no even-numbered-register rule (measured with Clang and
  GCC as black boxes). A class holding one is placed the same way.
- Not yet:
  - A 128-bit integer through an ellipsis, or named in a variadic function on x86-64 (#1247).
- **Enumerations**: retain extended integer values and use them in fixed underlying types.
  The modes select the unfixed underlying type and overflow behavior documented in
  [the compatibility matrix](docs/compatibility.toml) (#1176).
- Tests: `own/int128` (types, constant evaluation, narrowing, specifiers, symbols against a Clang
  peer, native arithmetic against constant evaluation, and calls in both directions with a Clang
  peer), and `unit.linux_int128` runs the native ones on x86-64 Linux with GCC and Clang peers.
  `-Wpedantic` and `-Wno-pedantic` are accepted and select no warning, so the tests pass
  `-Wno-pedantic` for the oracles.

## Templates, ζ step 8: template template parameters and `__make_integer_seq` (#88, #208, 2026-11-19)

Fable decided the representation (ast.md §6.7); the Opus session wrote the code.
- **Template template parameters** ([temp.param]/1): `template<…> class TT`, with a default and as
  a pack; their own head is a parameter list one level deeper (`TemplateParamSema.params`).
- **Template arguments that are templates** ([temp.arg.template]): a class or alias template, a
  template template parameter, `N::X`; `TemplateArg::templ`. An argument template fits a
  parameter when the parameter's own arguments, made unique, fit its head with its defaults
  (P0522), a trailing pack taking every remaining parameter of its kind: `template<class> class`
  takes `template<class, class = int> class`, `template<class U, U...> class` takes
  `template<class T, int...> class`, and `template<class...> class` takes `template<class, class>
  class`. A class template's injected-class-name is the template for a template parameter and
  the class for a type one ([temp.local]/1).
- **`TT<args>`** is a dependent type (`TDepSpec`), completed by the defaults of TT's own head,
  until TT is bound, then its template's specialization or alias (`Templates::apply_template`); TT
  is deduced from a class template specialization and from a template argument
  ([temp.deduct.type]/8), also in partial specializations and as a pack.
- **`__make_integer_seq<Seq, T, N>`**, Clang's built-in (docs/EXTENSIONS.md), with
  `__has_builtin(__make_integer_seq)` answering 1: libycxx's `make_integer_sequence` now reaches it.
- **Mangling** as Clang's: a template argument by its name, `T_I…E`, `18__make_integer_seq`, and
  Clang's `Tn`, `Tp`, `Tt…E` and `TL…_` prefixes on a function template's own arguments (for a
  pack, by its first element or its emptiness), which also corrects `template<class T, T N>`
  specializations (`_Z1fIiTnT_Li3EEiv`).
- Tests: `tests/own/templates/t8-*` (15, 8 from the review; a PEER test links Clang's
  definitions), 3 more draft examples pass. `<cstdio>`: 16 → 8 errors, all pointer-to-member types (#238).
- Not yet: constrained template template parameters (#271); partial ordering of partial
  specializations through template template parameters with different heads, the last step of
  [temp.arg.template]/4, and `__make_integer_seq` as an argument (#272); the diagnostics of a
  template-id before `::` whose arguments do not fit (#275); member templates and `Q::template M`
  as an argument.

## sizeof and alignof of classes (#194, 2026-11-19)

- One layout rule, two readers: `ast::record_layout` (src/ast/target.cppm) gives a class's member
  offsets, size and alignment from its members' sizes and alignments (Itanium C++ ABI §2.4, no
  bases or virtual functions yet). The back end's `QueryLayout::class_layout` reads it with the
  unit's `ClassShape`; `Sema::layout_of` reads it with the complete class's `ClassRec`, from which
  the shapes are built. A reference member is laid out as a pointer. Each reader computes a
  class's layout once (`Sema::class_places_`, `QueryLayout::class_layouts_` and `records_`), so
  nested classes cost linear time.
- `offsetof` ([support.types.layout], #248) uses the same record layout for direct non-static
  members in ordinary standard-layout classes and unions, producing a target `size_t`
  constant. `__has_builtin(__builtin_offsetof)` reports availability.
- Bases, a vptr and empty-base placement extend `record_layout`'s input when inheritance and
  virtual functions arrive.
- Ordinary named namespace-scope class heads accept concrete `alignas` values and type operands
  for requests up to 16 bytes ([dcl.align]). Repeated requests use their strictest value. The
  definition checks annotated forwards, natural member and base alignment, and layout attributes
  whose effect is unresolved. Both layout consumers apply the validated value to object size,
  array stride, containing records and storage. Darwin diagnoses by-value use when the class or
  a contained value has strengthened alignment.
- A leading `alignas` on a non-static object member accepts requests up to 16 bytes, including
  dependent operands replayed with the class's template arguments. An alignment request applies
  to the field offset and containing record without changing the field's type or array bound.
  Other placements, packs, wider requests and further ABI cases remain in
  [#303](https://github.com/yrashk/ycxx/issues/303).
- Tests: `tests/own/classes/s7-sizeof-class.accept.cpp` (padding, nesting, arrays, unions,
  references, empty and template classes), `s7-sizeof-incomplete.reject.cpp`,
  `s7-sizeof-deep.run.cpp` (22 levels of nesting), `tests/own/abi/sizeof-class.run.cpp` (agrees
  with Clang's sizes and member offsets).

## Resident project builds (#1251, #1255)

`ycxx build <target>` reads `build.ycxx` in the workspace and publishes a successful
native executable under `.ycxx/out`. The first rules are `cpp.executable`,
`cpp.library` (a source-backed object group), and `generate` (a declared host
tool with staged inputs and named outputs). Explicit source globs are resolved on
each request. Linux uses the native freestanding compiler and linker.
The manifest identifies the daemon scope before the first build creates `.ycxx`,
including when the workspace is nested in a checkout (#1305).

`ycxx build <target> --target=<triple>` selects the standalone compiler and
linker's native x86-64 Linux or arm64 Darwin target (#1345). An explicit
selection publishes under `.ycxx/targets/<triple>/` and scopes generated-file
overlays, rule snapshots, runtime assets and watch state to that configuration.
Source-built rule packages and declared tools execute on the host. Their
resident receipts can be shared across target configurations.

In project action arguments, `target_triple()` resolves to one canonical
argument word for the selected compiler target, including the host default
(#1347). That word enters the action receipt key. The same resolved words and
inputs can share a receipt across default and explicit requests. The planner
also resolves it in generated planning inputs and rule witnesses. Rule value
exports and their consumers are scoped by triple. Package bootstrap recipes
continue to describe host builds. Inspection shows the argument source and
the action trace shows its position and resolved word.

Project generators and rule witnesses can use `host_triple()` in their
arguments to get the canonical host triple independently of target selection
(#1534). Receipts key on the resolved argument, so builds for different
targets can share an action with identical resolved arguments and inputs.
Inspection and traces identify the host selector.

Project generators and rule witnesses can also use both selectors in
their `environment` records (#1540). The resolved strings enter the action
identity and can share receipts with equal literals. Inspection reports
each resolved environment value and its selector origin.

A resident workspace query asks for generator receipts and fetches the native
compiler under the same query context. The coordinator admits tool results between
epochs. Each generated path is a separate input, so unchanged sibling bytes cut
off compiler work. Ordinary include probes continue to use the daemon file table,
including missing candidates. The existing trace carries the native query tree,
and reports external tool execution between workspace query attempts.

`ycxx build --watch` uses the compiler watch loop and live trace browser
(#1263). The daemon watches native compiler inputs and reobserves the project
plan between runs for changes to the definition, generator inputs, and glob
matches. Rule package source, compiler discovery and declared reads also enter
the observation. It continues after a failed run.

`ycxx show <target>` uses the same resolver without executing any actions
(#1256). It shows verbatim source, resolved facts with their declaration
locations, or a semantic diff against a saved JSON snapshot. The HTML report
includes the resolved plan and its definition.
Use the same `--target` option to read the chosen variant and its target
provenance. The trace carries the compiler target for the request.

Schema-1 JSON plans include a typed `dependency_graph` of resolved library
and named artifact uses (#1453). Each edge gives the consumer, producer,
planner field, and manifest location. Artifact edges name the output, and
named input edges also give the consumer binding. Unresolved rules can yield
an incomplete graph with the edges resolved so far. The HTML report shows
these records under **Dependencies**, with links into the manifest.

The `dependency_graph.value_edges` array records resolved typed export
bindings (#1477). A value edge names the consumer, usage (`capabilities`
or `facts`), binding, producer, and export, with the manifest location of
the `value(...)` reference. Graph completeness applies to both edge arrays.
HTML plans and comparisons show current edges under **Value bindings**,
with source links.

`ycxx show --against` compares occurrence counts by consumer, usage,
producer, output, and binding when both plans have complete dependency
graphs (#1464).
JSON includes before and after edge records for each changed count. HTML
shows those counts under **Dependency changes** and links current sites to
the manifest. Moved references preserve the count, and older snapshots
without a dependency graph can still compare plan facts.

When both plans have complete graphs with `value_edges`, `ycxx show --against`
also compares typed binding occurrence counts by consumer, usage, binding,
producer, and export (#1493). JSON records before and after occurrences in
`value_binding_changes`. HTML shows counts and source sites under
**Value binding changes**, with links for sites in the current manifest.

The **Target dependencies** panel in standalone HTML plans and comparisons
starts at the selected target (#1499). Artifact and value uses link to the
producer's card and to their sites in `build.ycxx`. Shared producers have
one card, and incomplete plans show the links resolved so far.

The explorable **Graph** in standalone HTML plans and comparisons opens
at the selected target and its immediate inputs (#1604). Its buttons expand
dependencies and dependents, with one node per target. The sidebar links
the selected target's facts and authored edges to `build.ycxx`. Targets
with identical visible labels keep distinct details (#1611). Incomplete
plans also show an entry point and its resolved inputs in each component
disconnected from the selected target.

`ycxx show --targets` catalogs declared targets in the working directory's
`build.ycxx` (#1392). The read-only manifest parse lists names in byte order
with their declared rule kinds and source locations. JSON includes the same
records with schema 1.
HTML links each record to its declaration line in the displayed manifest.
The catalog accepts a regular UTF-8 manifest and follows symlinks to one (#1441).

Named-module discovery runs before native semantic analysis through
`ModuleDirectives` and `ModuleGraph` (#1349). It follows the marked module
and import directives in each unit's selected preprocessing view. The graph
omits occurrence locations, so moving a directive can refresh provenance
without changing the dependencies. Read-only plans show physical source
locations and mark scans that probe declared generated outputs as pending
(#1354). Build traces include these units once the virtual inputs are available.
Named-module semantics and BMI support are tracked in #319.

`ModuleProviders` resolves written imports within the reachable module
graph before native compilation (#1353). It reports missing, unique and
ambiguous providers and finds cyclic components among unique edges.
`ModuleProviderReport` and read-only inspection add physical import and
provider locations. A location-only edit can update the report while
reusing provider resolution. Incomplete read-only scans make missing or
unique matches provisional.

`ycxx show <target> --format=json` includes an optional `module_graph` for
resolved plans under a native configuration (#1363). It records configured
translation units, scan states, named directives with physical source locations,
provider candidates, conflicts, and cyclic components. Its unit IDs are local
to each snapshot.
Schema-1 plan comparisons continue to use semantic facts.

The standalone HTML plan renders that inspection data under **Module graph**
(#1378). Configured translation unit panels group declarations and imports with
physical locations and links to candidate declarations. The section identifies
incomplete scans, provider conflicts, and cyclic components. With `--against`,
it shows the current plan's graph below **Plan changes**.

JSON plan comparisons include `module_changes` when both snapshots contain a
module graph (#1383). Units match by logical source and ordered include paths,
so provider changes can be compared across target variants. The comparison
reports changes to units, imports, conflicts, cycles, and scan completeness.
Body-only edits and moved source sites preserve the semantic results. Older
graphs can use source-only matching when their unit paths are unambiguous.

HTML comparisons show these semantic differences in **Module changes** when
both plans contain graphs (#1390). The report shows before and after provider
locations, resolution and scan states, conflicts, and cycles. After entries
link to the corresponding imports, declarations, and units in the current
graph. The section identifies source-only matches and explains when an older
graph lacks enough configuration information to compare. An available
comparison with equal semantic results reports no module changes. Earlier
schema-1 snapshots without a graph still show **Plan changes** and the current
**Module graph** when available.

The standard C++ and generator rule declarations use a compiled rule package.
`use plugin("tools/custom/rules") as cpp` supplies a different C++ provider. The
bundled and workspace packages use the same worker. `cxx()` in a package recipe
tries native ycxx first, then searches for host compilers. A `cxx("clang++")` pin
selects one compiler. Package compilation and worker execution appear in the
build trace, and inspection uses tracked admitted expansions. [#1281](https://github.com/yrashk/ycxx/issues/1281)
Rule packages share a resolved compiler's content fingerprint within one validation
pass. Discovery still follows each candidate's invocation path. Watch polls and
effect admission checks start fresh passes. [#1671](https://github.com/yrashk/ycxx/issues/1671)
Compiler discovery formats its filesystem diagnostic to avoid a GCC 16 Release false positive
when inserting a prefix into a temporary string.

`ycxx build app --probe-toolchain` measures the actual compiler selected for each
demanded `cxx()` package. The named observations distinguish support, rejection and
inconclusive evidence across target reporting, dialects, headers, language and
library features, objects, executable links, and shared-library compile, link and
load. The resident engine reuses typed receipts for unchanged inputs. Read-only
inspection shows the admitted observations and their provenance. [#1289](https://github.com/yrashk/ycxx/issues/1289)
On arm64 macOS, the native `link.host-shared` probe uses the freestanding Mach-O
dylib recipe. `link.host-shared.load` calls the rule entry point through the
worker. The [native leaf example](examples/project-build/native-leaf-rule/README.md)
pins ycxx for both its package and these measurements. [#1678](https://github.com/yrashk/ycxx/issues/1678)

The optional `builtin:capabilities` rule package consumes declared
`probe(package, id)` bindings and emits a named C++ header artifact. The worker
receives typed support states through the same ABI as workspace packages. A
reprobe with the same states keeps the generated bytes and uses the existing
worker, generator, and compiler results. The plan and trace retain the query
and profile behind each binding. [#1290](https://github.com/yrashk/ycxx/issues/1290)
The coordinator uses a named probe-receipt admission callback to avoid a GCC 16
module error when boxing nested lambdas.

A source-built package can emit a declared `action.observe` witness and
classify a normal exit through the optional isolated worker entry point.
`value("target", "field")` makes the consumer's arguments depend on the stable
typed export query. Equal support cuts off changes to the receipt. Watch and
read-only inspection revalidate the provider and the staged witness inputs.
Named admission callbacks avoid the same GCC 16 module limitation.
[#1257](https://github.com/yrashk/ycxx/issues/1257)

Package expansions can declare private actions feeding a public generator.
Named artifact inputs stage selected receipt bytes and modes into downstream
actions. Executables publish `runtime_assets` under `.ycxx/assets/<target>`
after a successful build. These outputs have their own receipt dependencies
and leave the native request reusable on shader edits. The sample packs real
SPIR-V and the portable daemon test exercises watch, failed compilation, and
restoration from receipts. [#1258](https://github.com/yrashk/ycxx/issues/1258)

Runtime asset publication stages a complete generation and switches the public
directory with one filesystem operation on Linux and macOS. The ownership
record moves with the assets, so the next successful build can retire removed
or renamed outputs while preserving unrelated files. Failed staging or exchange
preserves the last good asset set. Trace events report publication separately
from tool execution. [#1329](https://github.com/yrashk/ycxx/issues/1329)

A source-built rule can use `plan_inputs` to read a generated artifact while
planning its declarations. The coordinator stages the selected bytes from
the ordinary action receipt, and equal bytes reuse the worker result. Watch
observes the producer graph, while read-only inspection validates its inputs
and shows the selected receipt provenance. The shader example uses this to
add and remove modules from a generated inventory. [#1335](https://github.com/yrashk/ycxx/issues/1335)

Planning can compose cached rule results before validating the expanded
prefix. Cache misses reobserve the prefix before launching tools and again
before admitting their receipts. Staging follows the requested producers'
ordinary artifact dependencies, and validation retains the full planning
graph. Edits during a producer run discard its superseded result and retry
without replacing published outputs. [#1337](https://github.com/yrashk/ycxx/issues/1337)

On x86-64 Linux and arm64 macOS, `cpp.library` can export generated static
archives through `public_link_inputs` and `cpp.executable` can name direct
`link_inputs`. The planner orders libraries before their dependencies and
keeps explicit repetitions in the archive lists. The native linker reads
archive bytes from the ordinary action receipts. A changed archive runs the
link with reusable translation units, and equal bytes retain the link.
Inspection shows the declaring library, archive order, and artifact reference.
Watch preserves the last successful executable if the archive cannot be linked.
[#1339](https://github.com/yrashk/ycxx/issues/1339)

The foreign Make example builds its adapter as a C++ rule package. The worker
reads an explicit file inventory and emits one action that runs Make on the
staged C project. The ordinary named archive output enters the existing native
link through a library. File edits reuse the rule expansion, equal archive bytes
reuse the link, and a failed Make keeps the last good executable under watch.
[#1341](https://github.com/yrashk/ycxx/issues/1341)

Generated actions can declare named helper executables with `tool_inputs`.
Their absolute spellings, canonical paths, and content fingerprints enter
the action receipt key. Watch observes helper edits and symlink retargets.
Inspection reports current helper identity or availability without launching
the action. The Make adapter uses these declarations for its C compiler and
archiver. [#1343](https://github.com/yrashk/ycxx/issues/1343)

Rule witnesses also accept `tool_inputs`. A helper change invalidates the
witness receipt, and an equal classified value preserves downstream reuse.
The staged-witness walkthrough passes an absolute helper path to its
analyzer and watches helper-only edits. [#1526](https://github.com/yrashk/ycxx/issues/1526)

A rule-emitted generator can select an executable artifact with
`tool = input("name")` (#1505). The producer's named output is staged using
its bytes and executable mode, which enter the consumer's ordinary action
identity. Inspection reports the staged path and selector and links the
input edge to the producer. The staged tool example watches an executable
script that writes a generated C++ header.

A `cpp.host_executable` rule compiles disk sources and their includes with
the resident host compiler and linker (#1559). An ordinary generator can
stage `artifact("name", "executable")` as an argument or as its selected tool.
Target variants reuse the host request, and generator receipts use the bytes
and executable mode of the staged image. Watch follows the tool's C++ inputs.
Read-only plans show its host triple and the artifact dependency.
[Native host tool example](examples/project-build/native-host-tool/README.md)

A `cpp.host_executable` can link disk-backed `cpp.library` dependencies
through `deps` (#1576). Each executable's closure includes its transitive
public include directories and visits each library declaration once. The
same declaration can also serve the selected target app. Inspection shows
the shared declaration and each authored dependency edge.
[Shared library diamond example](examples/project-build/host-cxx-libraries/README.md)

A `cpp.host_executable` can consume `generated_headers` through ordinary
action producers (#1579). The resident host compile sees a virtual include
directory with the producer's output names and bytes. Equal header bytes
preserve the host compile, and real disk lookup observations continue to
drive watch. Normal builds, planning inputs, and witnesses share the
header producer receipts.
[Generated host header example](examples/project-build/host-generated-headers/README.md)

A `cpp.host_executable` can also use a generated artifact directly in
`sources` (#1583). The resident compiler reads the source and its quoted
generated siblings from the same host snapshot. Real disk includes remain
observed. Planning and witness snapshots retain a completed observation
even when the host tool read no disk source or include files.
[Generated host source example](examples/project-build/host-generated-sources/README.md)

A disk-backed `cpp.library` linked into a host executable can declare
`generated_headers` (#1588). Each library translation unit sees its own
and inherited generated directories, so an edit to one branch preserves
unrelated library compiles. A library declaration shared with the selected
target retains its distinct target include view. Plans show both include
paths and one dependency edge for the authored header binding.
[Generated host library header example](examples/project-build/host-library-generated-headers/README.md)

Host-linked libraries can mix generated C++ source artifacts and disk
sources (#1600). Each generated main keeps its own producer's siblings
available for quoted includes. The library's public search directories
continue to come from its headers and dependencies. Inspection shows
the host and selected-target source paths with one authored source edge.
[Generated host library source example](examples/project-build/host-library-generated-sources/README.md)

A `cpp.host_executable` can declare direct `link_inputs` from ordinary
host-independent action chains (#1616). The native link preserves operand order
and repeated archives under foreign target selection. Archive-only edits leave
translation unit inputs unchanged. Equal staged images reuse consuming actions,
including those for early planning and witnesses. Read-only plans show each
authored archive reference and its host path.
[Native host archive example](examples/project-build/host-static-archive-inputs/README.md)

A host-linked `cpp.library` can export ordinary generated archives through
`public_link_inputs` (#1622). The host link orders direct archives first
and emits a shared library's operands once in a diamond, retaining explicit
repeats. Host and selected links use the same bytes for a shared reference
and report their own effective paths. Archive-only edits reuse the host's
translation unit inputs.
[Inherited host archive example](examples/project-build/host-library-static-archive-inputs/README.md)

A `cpp.library.public_link_inputs` position can choose explicit `host`
and `target` artifact references (#1629). The host tool and selected app
resolve their own role, including when both links use the host triple.
They can inherit one library while using distinct archive formats, or
select two named outputs of one ordinary producer. Plans report each
active role, resolved path, and authored site.
[Archive roles example](examples/project-build/archive-consumer-bindings/README.md)

A `cpp.library.generated_headers` position can select explicit `host`
and `target` artifact references (#1637). A host tool and selected app
can share a library while seeing different versions of an include.
Each traversal keeps its own first-use include order and generated file
paths. Plan facts preserve both role-specific sequences and the authored
sites, including inherited directories whose positions differ.
[Generated header roles example](examples/project-build/role-bound-library-headers/README.md)

A `cpp.library.sources` position can select explicit `host` and
`target` artifact references (#1647). A native host tool and selected
app can compile different generated implementations from the same library
declaration, even when their triples match. Each implementation retains
its producer's quoted sibling lookup and authored position. Inspection
shows its logical source path, role, and artifact reference location.
[Generated source roles example](examples/project-build/role-bound-library-sources/README.md)

A witness can also use `tool = input("name")` to run a staged executable
artifact (#1515). The staged-witness example classifies the generated
analyzer's stdout into a typed score and generates a C++ constant from it.
An analyzer edit with the same score updates the witness evidence and
reuses the header and native build.

A typed witness can select a `cpp.host_executable` artifact directly as
its tool (#1566). The resident host build records source and include
lookups, including misses. Read-only inspection and watch check those
observations. The witness receipt uses the staged image, so an edit that
preserves the linked bytes refreshes the observations and reuses the
witness. [Native host witness example](examples/project-build/native-host-witness/README.md)

An ordinary generator chain can stage a host executable upstream of a typed
witness (#1571). The coordinator captures the reachable host images and
file observations together. Staged bytes and executable mode enter the
action keys. The witness receipt stores raw evidence, and the classifier
supplies the typed value. Equivalent artifacts or equal classified results
allow downstream reuse.
[Host producer witness example](examples/project-build/host-producer-witness/README.md)

Rule workers can read a generated inventory through `plan_inputs` when a
native host tool runs upstream (#1574). Host observations drive watch and
read-only staleness checks. Equal selected bytes reuse the rule worker and
refresh provenance. A rule that also classifies a witness revalidates the
planning reads and witness inputs before admitting the result.
[Host planning input example](examples/project-build/host-planning-inputs/README.md)

A source-built rule package can stage a named generated artifact into an
`action.observe` witness and classify it as a canonical table of typed rows.
`facts` bindings resolve the stable `value("target", "field")` query without
adding receipt provenance to the consumer's arguments. The shader reflection
example consumes the same fragment SPIR-V receipt as its runtime bundle and
generates a C++ header from the descriptor layout. The sample includes uniform
buffers and one-dimensional literal `sampler2D` arrays, with typed descriptor
kinds and element counts. Its executable checks the count in the bundled SPIR-V.
An equal layout preserves native work. Read-only inspection and watch reobserve
the producer graph, including the selected tools and input files.
[#1258](https://github.com/yrashk/ycxx/issues/1258)
[#1486](https://github.com/yrashk/ycxx/issues/1486)

## The engine trace (#245, 2026-11-19)

`ycxx cxx --trace-engine[=stream|interactive]` (or `YCXX_TRACE_ENGINE`) shows what a request
did in the query engine; `ycxx ctl trace [--interactive]` attaches to the daemon and shows every
request it serves.

- Engine (`src/engine/engine.cppm`): a request may carry an `engine::Tracer` (`Db::get`,
  `Db::request`, `Epochs::spawn`), held on its `Root` like its `Counters`, so concurrent requests
  never mix and a request keeps one trace through its restarts. Every activity on a cell goes
  through one call, `Db::note`, which counts `Stats` and gives the tracer its event, so the two
  cannot disagree: executed, cutoff (re-executed, value unchanged), verified (green after its
  dependencies), reused (verified at this revision), durable (the shortcut), input (a first
  read), recomputed, waited (contended), failed; with the parent's event, start and end times and
  the workers it started and ended on. Frames note before they publish, so children precede
  parents. Re-checks after a wait note only cells another frame owned (`Stats.hits` too). Keys render through a kind's `describe`, a key type's `trace_describe`
  (ADL), or their fields by reflection; values through a kind's `show`, when asked.
- Cost when off: one load and branch at each hook; `engine_bench` (Release, minimum of ten runs)
  measures the hit paths as before (41.4 ns a fetch hit, 16.9 ns a root `get` hit). Formatting
  stays out of the per-kind templates: in them it grew every engine user's unit until GCC stopped
  inlining the key hashing (+6 ns a hit).
- `ycxx.trace` (`src/trace/`): the collector (complete subtrees in pre-order, under a lock), the
  stream renderer (colors on a terminal without `NO_COLOR`, glyphs, indentation by demand), and
  the browser's model: the tree, filters (`words`, `is:<outcome>`, `w:<worker>`), the summary and
  the workers' timeline. `:browse` is the full-screen termios browser: tree with a detail pane
  (the value: L1 for `FunctionIr`, L2 for `Lower`, instruction words for `Codegen`), timeline
  with a time cursor, worker selection, live feed.
- The browser's layout (#1119): a title bar with the tabs and the live state, the summary (at
  most three lines), a line of toggles that show their keys and state (clickable), and a footer
  of the view's keys with `? help` and `q quit` pinned right; `?` is a scrolling box. Lines are
  fitted by priority (`pack`, `spread`, `ellipsize` in `:view`), so nothing runs off at 80
  columns or more. The timeline groups workers in the same cell (`w0–w17`) and cuts the demand
  chain in the middle; the pane wraps at spaces and `/`, and shows memory as own / in all.
- Names: units and files by their file, functions by qualified name, requests and programs by
  their units and output (`Build(a.cpp → a)`).
- Errors: a compile error is a value, so a kind says which of its values hold errors
  (`errors(value)`). Events carry the text; the cell where errors start (no child has any) is
  marked, with its message, and every cell above names it (`error below:`, JSON `error_at`).
  Preprocessing, front-end and back-end diagnostics include the source file, line and byte column
  where available (#404). The trace keeps error-severity messages only, so a `#warning` is no error.
- Values shown: L1 (`FunctionIr`), L2 (`Lower`), and assembly (`Codegen`, `Optimized`) from
  `aarch64::disassemble`, which decodes with the encoder's own form table.
- `--trace-engine=json`: JSON Lines on stderr (events with values and errors, the diagnostics and
  exit status, a summary); every line parses. `ctl trace --json` likewise.
- An input's first read is `input`, later reads `reused`; identical instants in a row under one
  parent are folded (`×3`, JSON `repeat`).
- Daemon: a `compile` that asks gets `trace` frames before its answer, written by its connection
  thread; a `watch` connection gets every request's, through a queue its own thread writes (a
  watcher 64 MiB behind is dropped), and is shut down with the daemon. A cancelled request's runs
  are waited for (`Epochs::Ticket::wait_idle`), so nothing they use goes before they end.
- `ycxx cxx --watch`: builds, then sends `await_change`, which the daemon answers once a batch
  changes a file-table entry (a file some build read; build outputs and other files are not
  entries), and builds again. With the stream each build is traced under a header; with the
  browser the builds run on a thread of their own and each is a live node holding its
  diagnostics. A daemon that stops leaves the wait unanswered, and the next build starts a new
  one. The wait first lists the files it watches (`watching`: the file table's existing entries).
- Tests: `unit.trace` (a golden stream of a unit built cold, again and after a whitespace edit;
  the collector under 8 threads; the tree, filters, summary and timeline); `daemon.trace` (traced
  builds through the daemon, a watcher, a stalled watcher beside 40 builds, shutdown).
- The browser (#635): `:view` is its model, with no terminal: state, keys, mouse, and frames for a
  given size. `:browse` runs it on `/dev/tty`. It uses SGR mouse modes 1000, 1002 and 1006 with
  autowrap off, and draws each line at its row. A `SIGWINCH` self-pipe redraws on resize. An
  interrupted terminal wait also checks the size, so a deferred handler cannot stall the redraw
  (#1375). Fatal signals restore the terminal, then re-raise. The tree has columns (time, worker,
  alloc and retains, summed over the subtree) whose headers sort it (`Tree::Order::time` added). The
  summary wraps on narrow screens. The divider drags (or `<`/`>`) and keeps its share of the
  width. Tests: `unit.trace` (SGR parsing, hit-testing, click, double click, marker, wheel,
  divider drag, sort, focus, wrap; frames against `browser.golden`); `daemon.trace_tui` (under a
  pty: modes on and off, a click, a resize, SIGTERM and SIGABRT restoring the terminal),
  mutation-checked.
- Copy (#636): `y` copies the cell (the pane's text, unwrapped) and `Y` its subtree (stream lines
  with times and memory). A drag over rows or pane text, or `V` with j/k, selects, and the copy
  happens on release or `y`. `Action::copy` carries the text. `:browse` writes OSC 52 (plus
  tmux's passthrough form) and, locally, runs `YCXX_CLIPBOARD`, or writes the native clipboard
  (`:clipboard`: macOS NSPasteboard through the Objective-C runtime's C entry points, libobjc and
  AppKit loaded at the first copy; pbcopy if it refuses), or runs the first of wl-copy, xclip and xsel (Linux, until
  #730). Tests: `unit.trace` (the exact text of each way to copy, base64, OSC 52, a named
  pasteboard written and read back on macOS); `daemon.trace_tui` (`y` and a row drag through
  OSC 52 and the command).
- Histograms (#637): `Metric` (self time, the span less the union of the children's; time in
  all; allocated; peak; retained; allocations; runs; cache hits; waits), `metrics_of`,
  `self_times`, `Groups`, and `histogram(tree, Grouping, within, order)` (by kind, file or
  children; the tree's filter applies). The browser's histogram view has metric switches,
  grouping, scope, sort, and a bar that opens its cells in the tree (`kind:`/`at:` filters,
  or selection). The JSON summary has `histogram.by_kind`/`by_file`. The browser shows a
  cell's source in the pane, an `S` column, and `p` copies `path:line:col`. Tests:
  `unit.trace` (aggregation values, JSON, the view's frames against `histogram.golden`, its
  hits and keys, the source line, column and copy; self time mutation-checked).
- Source spans (#638): `engine::SourceSpan` on each `TraceEvent`, from a kind's `trace_span(const
  Db&, const Key&)`, which runs only in `Db::trace`. `Db::peek` reads an input, or a value
  verified at this revision, without a dependency. A file's and a unit's cells are the file; a
  function's cells (FunctionSourceQ to Codegen) are its definition's first to last token
  (`item_span`, lines as diagnostics count them). The JSON `span`, `at:file[:line[:col]]` in the
  filter, and `span_text`/`span_start` are in place. Tests: `unit.trace` (the spans of a real
  build, cells without spans, text, filter, JSON; mutation-checked) and `daemon.trace` (spans
  across the socket).
- Live (#939): `Tracer::starts`/`started` report a frame's start where it opens its span (the
  three span sites, cold, for tracers that ask: JSON and untraced builds pay nothing).
  `trace::Live` buffers starts and ends under a lock held for the append alone; `trace::Tee`
  feeds a batch collector and a live one. `Tree::apply` places events in any order (a child
  before its parent waits, unseen, until it comes or `settle`; children in id order; identical
  instants folded and refolded locally around a change), keeps the summary incrementally, and
  caches its rows per `version`; `Tree::add` places batches as they come. The daemon sends live
  `trace` frames (`TraceBatch::live`, `sent_ns`, the memory so far) on the request's tick to a
  client asking `compile::Trace::live` and to watchers asking `WatchRequest::live`. The client
  runs a browsed compile on a thread of its own (`browse_compile`), and the browser draws on a
  timer, only when something changed, with following (`f`). Tests: `unit.trace` (starts, ends,
  orphans, `is:running`; 180 simulated streams in the engine's order, shuffled and backwards,
  every step's sums, depths, folding and summary checked, each ending equal to the batches'
  tree, view and frames; two requests interleaved; following), `daemon.process` (`live_trace`:
  a build parked at its checkpoint is already in the browser through the daemon; mutation-checked),
  `daemon.trace_tui` (the first frame is live).
- Not yet: the analysed tree as a value, and stages linked to source (#254); per-resumption
  spans, so a frame's waits are not drawn as work (#255).

## Engine memory (#274, 2026-11-19)

What the process allocates, attributed to the engine's cells, in the trace and the daemon's status.

- `ycxx.memory` (`src/memory/`): the program's replacements of the global allocation functions
  (`new.cpp`, an object library linked into `ycxx` and `unit.engine_memory`; libycxx unchanged:
  its defaults are weak and its allocation table forwards to them) count requested sizes; an
  unsized delete asks `malloc_size`/`malloc_usable_size` (`:usable`, one file per platform).
  Process totals are batched per thread (64 KiB, flushed at thread exit too); a `Window` is a
  build's peak. `memory::disable()` stops counting: clients, `ctl`, `lsp`, and untraced in-process
  compiles call it.
- Engine (`src/engine/engine.cppm`, "Memory"): each `Frame` has a `memory::Usage`, the thread's
  target while the frame's code runs. It is set where the code starts or resumes (`run_frame`,
  `Settle::resume`, cheap execution and validation, a provider) and cleared after each work item,
  before a frame can be resumed elsewhere, and in `Root::release`. Child coroutine frames, `Wait`
  records, the engine's tables and the trace's strings count for no cell. A cell's `retained` moves by
  each run's net bytes measured after `store` (first allocator pays; an early cutoff drops its new
  value first); inputs from a provider are measured, inputs `set` from outside are walked
  (`heap_footprint`). Kinds sum them (`Db::memory_per_kind`). `TraceEvent` carries
  `alloc_bytes`, `allocs`, `peak_bytes`, `retained_bytes`.
- Trace (`src/trace/`): rollups over subtrees (retained once per cell, where it settles), sums per
  kind, the stream's columns and `memory:`/`by kind:` summary lines (off with `times`, so the golden
  is unchanged), the JSON fields and summary (`docs/TRACE.md`), the browser's columns, pane, header
  and `s` sort. A traced build's end batch carries the process's bytes and its window's peak.
- Daemon: `Status` has `memory_current`, `memory_peak`, `memory_allocated`, `allocations`,
  `memory_retained`, `memory_unattributed` and `retained_by_kind`.
- Cost: hit paths unchanged (no target store on a fetch that does not wait; `engine_bench` in
  Release, minimum of ten interleaved runs: fetch hit 42.3–43.7 ns against `main`'s 42.5–43.1,
  root `get` hit 17.5–17.6 ns against 17.1–17.3). A new/delete pair costs 22.4–23.0 ns with
  counting off and about 26 ns with it on, against 24.6–27.6 ns for libycxx's Debug defaults.
  Debug compiles measured about +2% CPU in paired runs on a shared machine, with counting on or
  off (#284). GCC on macOS uses emulated TLS for the thread's record.
- Tests: `unit.engine_memory` (known amounts, a cutoff, inputs set and provided, nested async and
  cheap cells, 32 frames resumed across workers under three schedules, process totals and a
  window), mutation-checked; `unit.trace` (rollups, lines, JSON, order, summaries); `daemon.trace`
  (the JSON memory schema, the stream's summary, `ctl status`).
- Not yet: drift of retained bytes over a long daemon session, a peak per request, and small
  inexactness (#284).

## Inheritance, step 1: non-virtual bases (#308, 2026-11-19)

- **Base-clauses** (`base_clause` in parse.cppm): each base is a complete class other than the
  class itself, named once, and not a union. Its access defaults to private for `class` and public
  for `struct`. `Program::set_bases` records the bases when the class head is analysed, and
  `class_def` copies them into `ClassRec::bases`. A primary class template supports one
  nonvirtual, nonpolymorphic direct base. It checks a nondependent base at definition and
  substitutes a dependent base at the first completeness demand. Its concrete specialization publishes
  the resolved base before layout or qualified lookup. [Design](docs/design/class-template-bases.md)
  Base packs and additional class-template base forms remain in #308. Virtual bases are #310.
  An explicit specialization's base-clause is an ordinary class's, and a partial
  specialization's is a primary template's, substituted with the arguments deduced for it;
  another specialization of the same template can be a base (#1134).
  A base's template argument can be a value-dependent expression with no `VDep*` form
  (`BC<T::value>`, `BC<!bool(T::value)>`, `cond_t<!bool(B1::value) || sizeof...(Bn) == 0, …>`).
  `template_value` gives it the placeholder `VDepParam{dependent_depth, 0}`, and the base is parsed
  again with the arguments bound (`instantiate_base`). A pack expansion of such an argument
  (`PS<T::k...>`) and the same template-id outside a base (`BC<!T::value>::value` in a member) need
  #209's representation (#308).
  A base can be a decltype-specifier, or a type named through one (`decltype(e)::type`). A dependent
  one is parsed again for each specialization (`instantiate_base`). A class type's own
  cv-qualifiers are ignored; written ones are rejected ([class.derived.general]/2) (#1243).
- **`decltype(e)::`** as a nested-name-specifier ([expr.prim.id.qual]): `decltype_qualifier_ahead`
  and `qualified_type_ahead` in the parser. It names a type after `typename`, as a type-specifier,
  a parameter, a template argument or a base; in a statement, a member of a dependent one is a type
  only after `typename` ([temp.res.general]). A namespace-scope alias template whose type has such an operand with
  no representation (`typename decltype(select<T>())::type`, with a deduced return type) is read
  again when it is named with known arguments (`AliasCallPattern::Kind::unrepresented`). A class
  template's member declaration can call an earlier static member of the class, also with
  explicit template arguments: a deduced return type comes from the member's instantiation, made
  while the class is instantiated (`members_so_far`). That body cannot name a member declared
  later (a later member type should be found, #1287). Member alias templates (#1285) and a
  decltype-specifier as a mem-initializer-id (#1286) are missing. Namespace-scope function template signatures,
  variable template types and default arguments with an unrepresented operand stay dependent (#1269).
  Tests: `tests/own/templates/decltype-*`, `tests/own/inherit/decltype-base*` (#1243).
- **Functional casts as template arguments** (`function_type_argument_ahead`,
  `tentative_type_argument`): `T(…)` is a type-id only if it can be a function type
  ([temp.arg.general]/2, [dcl.ambig.res]/2). T is a simple-type keyword, a typedef-name, a class
  or enumeration name, a class template-id, a qualified type name or a `typename`-specifier.
  The token lookahead settles most shapes: `bool(x) || y`, `int(3) + 1`, `bool(T::value)`, `U(3)`
  and `typename T::type{4}` are expressions, and `int()` and `void(...)` are types. When the
  parentheses begin with a ptr-operator, a `(`, attributes or a type name, the argument is parsed
  tentatively as a type-id. It is one if that parse ends at the argument's end with no new
  error. Otherwise the parser's position, diagnostics and deferred-check counters are restored and
  the argument is read as an expression: `bool(&g)`, `int((n))`, `int(E{2})` and `int(auto(3))`
  are expressions, while `int(&)[3]`, `U(int)` and `int(*(int))` stay types (#1190, #810).
  `bool(S{})` with a conversion function parses, but conversion functions are #339. `R(A...)` with
  a type parameter `R` is #1186.
- **Layout** (`ast::record_layout`, Itanium C++ ABI §2.4):
  - bases first, in declaration order; an empty base at offset 0 unless another subobject of its
    type is there;
  - a component placed at the data size so far, so a class that is not POD for layout leaves its
    tail padding to the derived class's members.
  - "POD for layout" uses the Itanium C++03 rule in spec and GCC modes and in Linux Clang
    mode. Darwin Clang mode uses the trivial and standard-layout rule. An all-private class
    demonstrates the tail padding difference (docs/compatibility.toml).
  - Sema (`Sema::class_layout`) and the back end (`ClassShape::bases`, `pod`) read the same rule.
  - `ir::Step::base` names a direct base by its class entity; `QueryLayout::step` gives its
    offset.
- **Lookup** (`Sema::find_member`, [class.member.lookup]): a member of the class, else of its
  bases. One found in two base subobjects is ambiguous unless it is one static member. The object
  expression or `this` is converted to the declaring class (`Sema::to_base`).
- **Conversions** ([conv.ptr]/3, [dcl.init.ref]/5, [over.best.ics]/6): `derived_to_base`
  conversions, one per direct base on the way. A pointer conversion keeps a null pointer null
  (`ptr_cmp` and `select`). A base class object is copied from the derived object's subobject.
  Each conversion ranks as a Conversion, and the nearest base wins (`Ics::base_depth`,
  [over.ics.rank]/4.4). The base must be unambiguous and accessible.
- **Access** ([class.access.base]): a member of a base is reached through the conversion, whose
  every step must be accessible. A protected member may be named in members and friends of
  derived classes (not yet [class.protected]'s rule on the object expression).
- **Class-template qualification** ([temp.dep], [class.member.lookup]): qualified lookup
  returns the base specialization's original static member. The derived declarations hide
  inherited ones, and an accessible inheritance path is required. Unqualified lookup during
  template replay skips a dependent direct base.
- **Special members:**
  - Bases are initialized first, from mem-initializers naming them or by default
    (`MemberInit` with field `~0u`), and destroyed after the members (`DestroyMember` with field
    `~0u`).
  - An aggregate's elements are its bases, then its members (`TypeFacts::bases_of` for irgen).
  - The implicit destructor and copy and move constructors account for the bases.
- **Tests:**
  - `tests/own/inherit/`: layout, members and conversions, construction order, lookup and
    access rejects.
  - `tests/own/abi/inheritance.run.cpp`: layout, conversions and a Clang-defined base
    constructor and destructor, across ycxx and Clang objects.
  - Ten draft examples now pass.
- From the review:
  - A trivial copy of a record writes its nvsize bytes, none for an empty class
    (`ir::TypeLayout::data`). A copy into a base subobject therefore leaves the derived
    members in its tail padding alone.
  - A base reference binds to the base subobject of a materialized derived prvalue.
  - Types and enumerators of bases are found from derived classes, and while the class is
    being defined (`Questions::defining_bases`).
  - Protected members may be named through the naming class's friends.
  - `?:` and `==` reach the composite base type.
  - A base's const member deletes the copy assignment.
  - Merged with floating point: a non-empty base's members count toward a homogeneous
    floating-point aggregate, and a class whose data is all in its bases is not empty for the
    calling convention.
  - For ordinary non-template member bodies, unqualified type lookup finds injected class names
    in the class and its nonvirtual bases before searching the enclosing namespace. The same
    type through multiple base subobjects is accessible when any of its lookup paths is
    accessible. Distinct competing types are ambiguous. Qualified type names also find
    inherited injected names, including with a class-key or within a type's qualifier.
    Ordinary terminal `B::B` names the constructor. See
    [the lookup design](docs/design/injected-class-names.md) (#361).
  - Unqualified inherited aliases in ordinary non-template member bodies and in-class alias
    declarations check access to the named declaration. Equal designated types merge and an
    accessible base path suffices. Competing types are ambiguous. Static initializer access errors
    name the alias and its declaring class. [#1001](https://github.com/yrashk/ycxx/issues/1001)
  - Nontrivial copy assignment of base subobjects remains tracked in #360.

## Implicit copy assignment of class members (#360, 2026-11-20)

- Ordinary non-template classes without bases use member overload resolution for assignment.
  An implicitly declared copy derives its parameter qualifiers from the subobjects' signatures.
  Its synthesized body assigns scalar and class members in declaration order and returns
  `*this`. Unusable subobject assignments are diagnosed on use. Operator notation sequences
  the right operand first, including when it calls an explicit overload.
- Proven trivial operations still use the existing representation copy. The remaining special
  member, implicit move, and nontrivial base, array and union cases stay in #244 and #360.
  [Design](docs/design/implicit-copy-assignment.md)
- A user-declared copy or move assignment suppresses the implicit move constructor. A move
  assignment also deletes an otherwise implicit copy constructor. The resulting constructor
  availability determines call triviality. [#244](https://github.com/yrashk/ycxx/issues/244)
- Copy and move assignment defaulted on its first in-class declaration is described with the
  other defaulted special members above (#157). Deleted copy assignments participate in overload
  resolution. [#244](https://github.com/yrashk/ycxx/issues/244)
- Namespace-scope class templates accept a deleted copy assignment on the first in-class
  declaration, including with an lvalue or rvalue ref-qualifier. Specializations preserve
  deletion and diagnose a selected deleted overload.
  [#709](https://github.com/yrashk/ycxx/issues/709) · [#311](https://github.com/yrashk/ycxx/issues/311)
- Ordinary classes and class templates accept an unconstrained, nonvirtual deleted move assignment
  on its first in-class declaration. It affects implicit special members and remains a candidate
  after specialization. A diagnostic is required when it wins overload resolution.
  [#244](https://github.com/yrashk/ycxx/issues/244) · [#1417](https://github.com/yrashk/ycxx/issues/1417)

## Ordinary virtual declarations (#309, 2026-11-21)

- A non-template root class can declare ordinary virtual member functions and define them
  inline. A virtual destructor can be defaulted in class or defined there, including with
  `constexpr`. Sema checks inline bodies in the complete-class context and records virtual
  functions, polymorphism and virtual destruction. The existing class traits use those facts.
- Syntax checking accepts pointer and reference interfaces and by-value function declarations.
  The layout guard covers the class and enclosing records, including array elements. Uses that
  require unsupported virtual layout or emission receive diagnostics before object production.
- A non-template derived class can have one complete ordinary polymorphic base. Override matching
  follows the base chain through private members and hidden names, with checks for matching
  parameter types, qualifiers, return types and exception specifications. The class traits also
  recognize inherited polymorphism and implicit virtual destructors.
- Ordinary non-template virtual members can use `final`, including an override written without
  `virtual`. `final` and `override` can appear in either order on the same declarator. Each can
  appear once. A further override of a `final` declaration is ill-formed even when ordinary
  lookup hides the declaration. [[class.virtual]](https://eel.is/c++draft/class.virtual) ·
  [[class.mem.general]](https://eel.is/c++draft/class.mem.general) · [#309](https://github.com/yrashk/ycxx/issues/309)
- Ordinary named namespace-scope classes and structs can use class-head `final`, with or
  without a base clause. A base specifier naming a final class through its name or an alias
  receives the same diagnostic after the definition, including after a later bare declaration.
  `final` remains an identifier in class names and object declarators.
  [[class.pre]](https://eel.is/c++draft/class.pre) · [#309](https://github.com/yrashk/ycxx/issues/309)
- A derived constructor can name that base in an unqualified parenthesized or flat braced
  initializer. Syntax checking uses ordinary constructor overload resolution, defaults, access,
  and list narrowing. The base destructor must be usable and accessible in the derived
  constructor. The initializer's converted arguments remain in its semantic body for deferred
  expression checks. Evaluated construction, constant evaluation of the objects, and native
  emission of their constructors still require virtual layout support.
  [[class.base.init]](https://eel.is/c++draft/class.base.init) · [#309](https://github.com/yrashk/ycxx/issues/309)
- An ordinary non-template virtual member can use `= 0` in its class declaration. The token
  after `=` must be exactly `0` after preprocessing. Class completion tracks which final
  overriders are pure through the single nonvirtual base chain. `__is_abstract` reads that fact.
  The front end diagnoses definitions of abstract objects and members, allocation,
  materialization and by-value function definitions or calls. Nondefining namespace-scope
  `extern` declarations, pointer and reference types, declarations of by-value functions,
  base subobjects and the result of direct `decltype(make())` follow their ordinary rules.
  [[class.abstract]](https://eel.is/c++draft/class.abstract) · [[dcl.type.decltype]](https://eel.is/c++draft/dcl.type.decltype) · [#309](https://github.com/yrashk/ycxx/issues/309)
- On Linux x86-64, the `<exception>` syntax and object probes with libycxx `4183795d` now pass
  the dependent pack call at `deps/libycxx/include/ycxx/core/invoke.hpp:112`. The first diagnostic
  is an ambiguous `__invocable_` at line 178, column 37.
  [#890](https://github.com/yrashk/ycxx/issues/890) · [#468](https://github.com/yrashk/ycxx/issues/468)
- On Linux x86-64, an ordinary declaration-only root with fixed integer virtual signatures can
  consume a peer-owned object through a pointer or reference. Dispatch loads the selected slot in
  declaration order and passes the original receiver as `this`. Qualifying the call with the
  receiver's own class name calls the declaration directly.
- The same Linux consumer can query `sizeof` and `alignof` and read or write directly declared
  builtin integral members up to 64 bits through a pointer or reference. The shared Itanium
  layout reserves the vptr at offset zero and preserves the declared field indices.
  [Itanium ABI §2.4](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#class-types) · [#309](https://github.com/yrashk/ycxx/issues/309)
- A derived consumer class can use one public nonvirtual root base, at most one directly
  declared integral field, and at most one declaration-only override with the same return type.
  The primary base supplies the vptr at offset zero and the derived field is placed using the
  base's nonvirtual size and alignment. Pointer upcasts preserve null, references designate
  the base subobject, and virtual calls through either view use the root's original slot.
  Construction, destruction, copying, by-value transfer, and virtual table production remain
  guarded. [Itanium ABI §2.5.2](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vtable-components) · [#309](https://github.com/yrashk/ycxx/issues/309)
- On Linux x86-64, a unit can emit one ordinary out-of-line, non-inline virtual member of
  a supported root. The member follows the key function in declaration order and takes the
  existing receiver pointer, with ordinary access to the object's integral fields. The peer
  defines the key function and supplies the table and RTTI.
  [Itanium ABI §5.2.3](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vague-vtable) · [#309](https://github.com/yrashk/ycxx/issues/309)
- The Linux x86-64 producer admits one external, default-visible, non-template root with
  one public integral field and two directly written fixed integer virtual signatures. A unit
  that defines the first virtual out of line and non-inline owns the table and RTTI. It emits
  default-visible `_ZTV` and `_ZTI` objects and a weak `_ZTS` object in a read-only ELF COMDAT
  keyed by the name. The peer defines the non-key member.
  Mixed GCC and Clang checks verify dispatch and RTTI. Daemon edits verify table ownership
  and byte-for-byte agreement with a cold compile.
  [Itanium ABI §5.2.3–4](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vague-vtable) ·
  [#309](https://github.com/yrashk/ycxx/issues/309) · [#422](https://github.com/yrashk/ycxx/issues/422)
- Linux x86-64 default-initializes a plain automatic variable of the
  supported root type. The root has an implicit default constructor, a trivial destructor and
  no default member initializer. Lowering stores `_ZTV + 16` in the vptr. The table can come
  from the key definition in the same unit or from a peer. [#309](https://github.com/yrashk/ycxx/issues/309)
- Linux x86-64 can also default-initialize a plain automatic derived object with one public
  nonvirtual abstract primary base. The base has one public pure `int f()` slot and no data
  members. The derived class has its public override and one public `int` field without an
  initializer. Both classes have implicit constructors and trivial nonvirtual destructors.
  Lowering stores the derived `_ZTV + 16` in the primary vptr. The caller can access the local
  field and bind a reference to the offset-zero base subobject. The key can be defined in the
  same unit or a peer. [#309](https://github.com/yrashk/ycxx/issues/309)
- The Linux x86-64 derived producer emits the table and RTTI for that shape when the unit defines
  the override out of class and non-inline. The global 24-byte `_ZTV` has one function slot.
  The global 24-byte `_ZTI` points to the base descriptor, a weak 16-byte object grouped with its
  relocations. Both NUL-terminated type names are weak objects with separate read-only COMDAT groups.
  [Itanium ABI §2.9.5](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#rtti-layout) ·
  [#309](https://github.com/yrashk/ycxx/issues/309) · [#422](https://github.com/yrashk/ycxx/issues/422)
- On Linux x86-64, a caller can dispatch through a pointer or lvalue reference to a
  peer-owned abstract root. The root is an ordinary, externally linked class with no
  bases or fields and one or two public, directly written pure virtuals. Each slot has
  an `int` or `unsigned int` result, at most one argument of those types, optional
  member `const` and bare `noexcept`. The caller uses the address point in the
  supplied subobject and passes that same pointer to the selected slot.
  `unit.linux_pure_virtual_dispatch` checks GCC and Clang peers, a secondary base,
  conditional evaluation, the consumer ELF symbols and cold/resident output across edits.
  [Itanium call ABI](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vcall.caller) ·
  [#309](https://github.com/yrashk/ycxx/issues/309) · [#659](https://github.com/yrashk/ycxx/issues/659)
- arm64 macOS has each of the virtual shapes above that Linux x86-64 has, with the same
  front end checks and the same lowering: calls on peer-owned roots, primary bases and pure
  interfaces, the non-key member, automatic roots and derived objects, and the key-owned root
  and derived tables with their RTTI. The Mach-O producer writes `__ZTV` and `__ZTI` to
  `__DATA,__const` and the weak `__ZTS` to `__TEXT,__const`. The base class's descriptor and name,
  which every user emits, follow Apple's arm64 convention for non-unique RTTI: weak private
  external, with bit 63 set in the descriptor's name pointer so `type_info` compares them by
  content across images. ycxx's Mach-O linker carries such a pointer's top byte in the chained
  rebase's `high8`. `unit.darwin_virtual` links the linux_* virtual fixtures with Clang peers,
  by ycxx's linker and the system linker. [#650](https://github.com/yrashk/ycxx/issues/650)
- Broader construction and table emission, pure-slot signatures and covariant returns remain in
  #309. Pure declarations and `final` for virtual destructors and assignment
  operators also remain there. Checking `operator=` includes matching implicitly declared copy
  and move assignments. Virtual destructor entry points, RTTI expressions, virtual bases and
  native class ABI have their own issues.
  [#309](https://github.com/yrashk/ycxx/issues/309) · [#136](https://github.com/yrashk/ycxx/issues/136) ·
  [#294](https://github.com/yrashk/ycxx/issues/294) · [#310](https://github.com/yrashk/ycxx/issues/310) ·
  [#489](https://github.com/yrashk/ycxx/issues/489)
  [Design](docs/design/ordinary-virtual-declarations.md)

## Evaluated immediate virtual calls (#1097)

Fieldless completed local objects with one nonvirtual base can dispatch immediate
virtual calls in constant evaluation. The checked receiver path selects the final
overrider. Explicit qualification selects the named implementation. Default arguments
come from the statically selected declaration. An unqualified call requires a known
dynamic type. Immediate members remain absent from native vtables.

Construction and destruction phases, persisted base references, and richer receiver
layouts remain in [#1097](https://github.com/yrashk/ycxx/issues/1097).

## Evaluated ordinary virtual calls (#322)

An ordinary virtual call with a supported scalar or void result can dispatch through
a complete local polymorphic object in constant evaluation, including through a
base reference in a constexpr helper ([class.virtual], [expr.const]). The object
has completed implicit default construction and has implicit trivial destruction.
This supports fieldless externally linked namespace-scope classes with no base
or one direct public nonvirtual fieldless root base. The statically selected
member supplies the arguments' defaults. Its final overrider must be constexpr
and defined before the evaluation, including when the statically selected
member is pure or lacks a constexpr specifier. Explicit qualification selects the
named function. Both native targets keep ordinary vtable dispatch.

Construction and destruction phases and broader receiver layouts remain tracked in
[#322](https://github.com/yrashk/ycxx/issues/322) and
[#25](https://github.com/yrashk/ycxx/issues/25).

## Virtual functions: general single inheritance (#309, 2026-11-24)

- Both targets compile, link and run ordinary non-template polymorphic classes with external
  linkage, at namespace scope or nested in such classes, with no base or one public nonvirtual
  dynamic base (`sema::DynamicClass`, computed at class completion). The peer profiles above keep
  their classes, and a profile class is not a dynamic base.
- Layout: a root's vptr at offset zero, a derived class sharing its primary base's.
  `ClassRec::virtual_layout_unsupported` now guards layout instead of
  `layout_requires_virtual_support`, so classes containing dynamic subobjects lay out too.
- Construction: a dynamic class without user-declared constructors, or a class with a dynamic
  subobject, gets an implicitly defined default constructor (`Program::synthesize_constructor`,
  body from `BodyAnalysis::implicit_default_constructor`) and an implicit copy constructor that
  copies a dynamic base with the base's own. Value-initialization zero-initializes first. Every
  constructor of a dynamic class stores its table's address point after its base subobject and
  before its members; every destructor stores it before its body ([class.cdtor]). A virtual
  destructor defaulted in class gets the implicit body.
- Entries: constructors and destructors of dynamic classes emit C2/D2 at the C1/D1 code, and
  each virtual destructor definition adds D0 (D1, then sized `operator delete` of the class size).
- Calls: an unqualified call of a virtual member loads the slot its declaration has in its own
  class's table; `C::f()`, with an explicit object or implicit `this`, calls directly.
- Out-of-class definitions of virtual members and virtual destructors.
- Tables: a unit defining the key function out of class and not inline owns strong `_ZTV`/`_ZTI`;
  a class without one gets weak ones (ELF COMDAT groups; Mach-O weak, non-unique RTTI for weak
  descriptors) where an emitted constructor or destructor installs the table, and a keyless base's
  descriptor alone where only a derived descriptor names it. Implicit constructors of dynamic
  classes are emitted where called. A destructor destroys a base defined elsewhere with its D2.
  Pure slots hold `__cxa_pure_virtual`; a derived descriptor is `__si_class_type_info`.
- Immediate virtual functions and overrides can be declared in the supported ordinary classes.
  An override must agree with its base declaration on `consteval`. Immediate declarations count
  toward polymorphism and pure final overriders. The vtable retains its header and contains only
  ordinary virtual entries. Immediate definitions are checked and have no native symbol.
  Evaluated immediate calls have the completed-local profile described above.
  Immediate virtual assignments remain tracked in #1097.
  [#1097](https://github.com/yrashk/ycxx/issues/1097)
- Not admitted (keep the virtual layout guard): classes in an unnamed namespace (#1155), fields
  declared through `decltype` (alias alignment, #1167), and a class serving only as a peer
  profile's abstract primary base keeps the profile's output (#1156).
- Tests: `tests/own/classes/virtual-dispatch` (13 run tests from the spec, two with Clang peers)
  on macOS; `unit.linux_virtual_dispatch` links the same tests with GCC and Clang on Linux
  (with `-fno-exceptions`: local unwind cleanups around throwing calls are #1104/#648's).
- Class template specializations are dynamic classes too (#1153): implicit ones from
  `Program::instantiate_class` (primary and partial patterns), explicit ones as ordinary classes
  with a key function. `complete_virtual_members` and `pure_final_overriders` serve both class
  definitions and instantiations; a pattern itself has no layout, is never abstract, and leaves
  `override`, return types and noexcept conditions to its specializations when its base is
  written there (`may_override_dependent_base`, `validate_override`). `instantiating_bases_` gives
  a specialization's members its substituted base while they are analysed.
- An implicit specialization has no key function: its table, RTTI and the members of its slots are
  weak. `instantiate_all` requires the slots' definitions only when the unit emits a constructor
  or destructor that installs the table, or a key function that owns it ([temp.inst]). Synthesized
  constructors and destructors of dynamic classes are walked and emitted where something uses
  them (`Program::emitted_on_use`).
  [[temp.inst]](https://eel.is/c++draft/temp.inst) · [#1153](https://github.com/yrashk/ycxx/issues/1153)
- Not yet: specializations for TU-local types (blocked in `dynamic_member`, #1155), classes nested
  in class templates (#1187), local and internal-linkage classes, non-polymorphic, non-public
  or multiple bases, `delete` through a virtual destructor (#293, #931), covariant returns,
  `typeid` (#294), broader constant evaluation of polymorphic objects (#322, #25),
  virtual bases (#310); follow-ups are listed in #309.
  [Design](docs/design/ordinary-virtual-declarations.md#general-single-inheritance-dynamic-classes-both-targets)

The driver and editor honor `-fno-rtti` and `-frtti` in argument order. `-frtti` restores the
default mode. With RTTI disabled, owned ELF and Mach-O vtables retain the same address point
and function slots and have a zero typeinfo word. The parser diagnoses
`typeid` and typed casts that require RTTI. Pointer identity and accessible unambiguous base
casts use ordinary conversions in either mode.
[#982](https://github.com/yrashk/ycxx/issues/982) · [Design](docs/design/freestanding-no-rtti.md)

## Borrowed `dynamic_cast` to `cv void*` (#294)

On Linux x86-64, `dynamic_cast<cv void*>(p)` accepts an ordinary complete polymorphic root
from the existing peer consumer profiles. The result is a pointer prvalue and retains every
cv-qualifier of the source pointee. The cast evaluates the pointer once and a typed null
operand yields a null result, including in constant evaluation. On the non-null path, the
vtable address point supplies the signed offset-to-top. Adding that byte offset to the
incoming subobject address recovers the address of the most-derived object, including when
the peer passes a secondary base. The cast introduces no RTTI symbols or calls.
[[expr.dynamic.cast]](https://eel.is/c++draft/expr.dynamic.cast) ·
[Itanium ABI §2.5.2](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vtable-components)

## Borrowed typed downcasts (#294)

Linux x86-64 can cast a borrowed root pointer to a pointer to its direct derived class.
The first profile uses two ordinary, fieldless, default-visible classes. The derived class has
one public nonvirtual primary base. Each class declares one public virtual `int` member with no
arguments, with the derived declaration overriding the base member. A hosted C++ peer
defines the two key functions. The consumer checks eligibility against the complete
translation unit and uses the peer's two RTTI objects and `__dynamic_cast`.
The result preserves or adds pointee cv. The cast evaluates its operand once and returns
a typed null for a null operand or a failed check. A null operand can be used in constant
evaluation. [Design](docs/design/linux-borrowed-typed-downcast.md) ·
[#294](https://github.com/yrashk/ycxx/issues/294)

The keyless-base profile uses a fieldless `B` with one public pure virtual `int f()` and a
`D` with the public override and one public `int` field without an initializer. Both classes use
the default-visible ABI. The consumer defines canonical weak `_ZTI` and `_ZTS` objects for `B`
in separate ELF COMDAT groups. An independently owned table in the same unit uses the same
base descriptor.
The complete translation unit must leave the derived key to the peer. Hosted linking supplies
the derived RTTI and `__dynamic_cast`.
[Design](docs/design/linux-keyless-base-rtti.md) · [#930](https://github.com/yrashk/ycxx/issues/930)

Keyless destinations, reference casts, `typeid`, non-null constant evaluation, and Darwin
parity are tracked in [#294](https://github.com/yrashk/ycxx/issues/294) and
[#659](https://github.com/yrashk/ycxx/issues/659).

## Deduced polymorphic lambda results (#569)

- A nongeneric lambda with a deduced value result can directly return a parenthesized
  polymorphic construction. Semantic analysis checks the selected constructor, converted
  arguments and potentially invoked destructor. The same-class prvalue initializes the
  result directly. An unevaluated call exposes the deduced result type.
- Ordinary `if consteval` checks both arms. A discarded `if constexpr` arm follows the
  existing instantiation rules. Invoking the lambda or converting it to a callable function
  pointer still requires native object lifetime and ABI support.
  [[stmt.return]](https://eel.is/c++draft/stmt.return) · [#569](https://github.com/yrashk/ycxx/issues/569) · [#309](https://github.com/yrashk/ycxx/issues/309)

## Templates, ζ step 9: member function templates (#88, #314, 2026-11-19)

- **Member function templates** ([temp.mem]) of classes and class templates: their head one level
  inside the class's; a call through an object or the class (`s.get(1)`, `S::st(1L)`) deduces
  their specializations as a call of a namespace-scope template does, overloads with ordinary
  members included; explicit arguments after `.f<…>`, `S::f<…>`, `N::f<…>`, an unqualified
  member template, or `.template f<…>` / `X::template f<…>`. Members overload on template-ness,
  template-head and constraints ([temp.over.link]/4-6).
- A class template specialization's member template binds the class's arguments as its
  enclosing level when it is instantiated.
- **Constructor templates**, `template<class It> V(It, It)`: function templates named as their
  class whose specializations are constructors; never `S(S)` ([class.copy.ctor]/5).
- **Definitions outside the class**, `template<class T> R S::f(T)` and `template<class U>
  template<class T> R W<U>::f(T)`, whose body is in the scope of both heads (`Program::outer_heads_`),
  matching a member template by its head and constraints; two heads before anything else are an
  error.
- Mangled as Clang: `_ZNK1S3getIiEET_S1_`, `_ZN1WIiE1fIcEEiT_`, `_ZN1SC1IiEET_` (the member's own
  parameters `T_` at any depth; a constructor without a return type).
- Tests: `tests/own/templates/m9-*` (13, 8 from the review; a PEER test links Clang's
  definitions); 5 more draft examples pass. Feature map: `temp-member` partial.
- Not yet (#340): member class templates (nested classes, #306), conversion function
  templates (#339), static data member templates (done since: see "Static data member templates";
  trailing requires-clauses on member templates came with #1417),
  `&S::get<int>`, explicit specializations of member templates; a non-type default depending on
  the class's parameter (#209). Found on the way: #345.

## Flat structural class template arguments (#317, 2026-11-24)

A directly named explicit class specialization accepts a braced flat structural
argument. The item splitter keeps grouped initializer tokens in the head and
records the real class body and its child scope. Equivalent named and braced
values select the same specialization. Native item checks and resident edits
cover the body boundary, key changes, and restoration. [#1248](https://github.com/yrashk/ycxx/issues/1248)

- Fixed-type parameters accept named complete flat aggregates with public nonmutable integral
  fields. The canonical argument records the class type and converted field values. Named
  constant expressions and equivalent braced values select the same specialization.
- The template parameter denotes a const object. Equivalent values share a parameter object
  across templates and translation units within the supported default-visible native scope.
  Linux x86-64 emits weak ELF parameter object definitions. The native Itanium name retains
  the argument type and value while omitting trailing zero fields.
- The Linux x86-64 controls link and run in both directions with GCC 16 and Clang 23.
  The fixtures cover named and braced values, specialization identity, direct address and
  mutable-field rejection in `tests/own/templates/`. Floating-point arguments, richer
  structural forms and the remaining dependent and visibility cases stay in #317.

Ordinary member aliases can project a structural parameter's public integral fields into
scalar template arguments. A guarded conditional uses its typed selected operand, so the
result selects the same specialization as the corresponding literal. The resident fixtures
check field-order and named-guard edits against cold compilation.

An alias can use `Box<detail::make<T, U>()>` when the immediate call returns a named
flat structural aggregate with public integral fields. Partial alias replay keeps the
qualified producer and its type arguments until the value is needed. Evaluation uses
the selected producer specialization and canonicalizes the result with equivalent
named and braced records. The resident fixture checks producer and specialization edits
against cold compilation. The Linux x86-64 ELF tests check the shared argument spelling
with GCC and Clang.

General member alias template replay remains in #317. Argument erasure during alias
substitution is tracked in #1311 and namespace-qualified explicit specialization
syntax in #1261. The pinned libycxx ratio header first reaches the separate named
static argument tracked in #1301 and #209.

## Member alias templates and `Q::template M` (#340, 2026-11-24)

- **Member alias templates** ([temp.alias], [temp.mem]): `template<class U> using apply = …;` in a
  class or class template is an `alias_template` entity scoped in the class (its head one level
  inside the class's), kept in `ClassRec::alias_templates` and never among the nested types, so it
  is a type only with arguments. A class template specialization's instantiation declares its own,
  with the class's arguments bound.
- **Named** as `C::M<args>` and `C::template M<args>` ([temp.names]/4), by its own name in the
  class and in classes derived from it, before `::`, in a functional conversion, and as a
  template template argument (`C::M`, `C::template M`).
- **Dependent qualifiers:** `typename Q::template M<args>` is `TDepName` with the `template_id`
  form and its arguments (ast.md §5.1); `Q::template M` as a template template argument is a
  `template_` argument whose type is `TDepName`'s `template_name` form. Substitution applies Q's
  member alias template once Q is a class (`Templates::member_template_of`); a template template
  parameter bound to `Q::template M` gives `typename Q::template M<args>`.
  GCC mode accepts the omitted keyword in a terminal member template-id of an explicit
  `typename` type.
- **Mangling:** `N <qualifier> <name> I…E E`, the template prefix a candidate, as Clang and GCC;
  `Q::template M` as a template argument follows the selected compatibility mode (DECISIONS §1.7).
- Access is checked where one is named; it hides a base's member of its name, and an earlier
  member hides a base's. A member defined outside its class template may name one through the
  current instantiation (`Y<T>::apply<int> Y<T>::f()`), the definition matching by
  `Templates::resolve_dep_names`.
- Tests: `tests/own/templates/member-alias-template*` (8, 2 from the review; a PEER test links
  the oracle's calls).
  `<iostream>` on macOS arm64: `'using' is not supported yet` 83 → 72 (the rest are #1150 and
  `using enum`), errors 3,237 → 3,165.
- Not yet: checking member alias constraints and checking namespace alias constraints with
  dependent, defaulted or pack arguments or during substitution in requirements
  ([#234](https://github.com/yrashk/ycxx/issues/234)). Access through a dependent name substituted
  in an alias template, as for a member type, remains open (#1199).
  Immediate calls in a member alias template's type
  still need per-use replay (#340).
  Member class templates, conversion function templates and the rest of #340 remain. Found on
  the way: #1191, #1192.

## Static data member templates (#340, 2026-11-27)

- **Declared** in a class or class template as `template<…> static T v = …;` ([temp.mem],
  [class.static.data]): a `variable_template` entity scoped in the class, its head one level inside
  the class's, kept among the class's static members with `is_template`. Its in-class initializer is
  a pattern; a class template specialization declares its own member template and skips the
  initializer ([temp.inst]/3). A non-static one is diagnosed ([temp.pre]/2).
- **Instantiated** per specialization used (`Program::variable_instance`, [temp.inst]/7): the member
  item is analysed again with the class's arguments and the member template's bound, through the
  static data member path, so `inline` non-constexpr ones have storage and `constexpr` ones are
  usable in constant expressions. `static constexpr auto` deduces per specialization; lambdas in
  initializers are owned by the specialization.
- **Named** unqualified in the class and in derived classes (initializers, member function bodies,
  default arguments, requires-clauses, `if constexpr`), as `C::v<args>`, `T::template v<args>`,
  `object.v<args>` and `p->v<args>`. One of the current instantiation is value-dependent. Access and
  constraints are checked.
- **Defined outside** a non-template class: `template<class T> const int N::w = …;`.
- **Emitted** with Clang's names (`_ZN1SIiE7counterIcEE`), strong, as other variable template
  specializations are until #124.
- Tests: `tests/own/templates/member-variable-template-*` (24; 17 by the spec-only author, a PEER
  test links Clang's uses, two XFAIL for the gaps below).
  `<iostream>` on macOS arm64: errors 2,468 → 2,423 at `07e6d567`; `basic_string.hpp` 156 → 111.
  Its remaining `__sv_like` errors are `__is_convertible` gaps (#1399, #218).
- Not yet: partial and explicit specializations (#1594); defining one of a class template outside
  the class (#256). Found on the way: #1595 (an undefined specialization's constant use reports an
  internal error).

## Placeholder types, θ step 1 (#279, 2026-11-19)

- **Types:** `TAuto` in `types` (ast.md §5.1) carries the placeholder kind, resolved concept entity
  and canonical written argument list. `TypeKind::placeholder` in sema and
  `Types::has_placeholder` find a placeholder under pointers,
  references, arrays and a function's return type. The parser reads `auto` and `decltype(auto)`
  as type specifiers, and trailing return types `auto f() -> T` ([dcl.fct]/2) with the
  parameters in scope.
- **Deduction** (`Sema::deduce_placeholder`, [dcl.type.auto.deduct]): `decltype(auto)` is the
  initializer's decltype, alone or diagnosed; `auto` in any declarator form is replaced by an
  invented type parameter (`Bindings::placeholder`) and deduced by `Templates::call_pair` and
  `deduce`, as from a call. A type-dependent initializer deduces `Builtin::dependent`, and each
  instantiation deduces again.
- **Variables:** block scope (`= e`, `(e)`, `{e}`; the name is declared first, so its own
  initializer cannot use it), namespace scope and variable templates (`= e`, `{e}`), static data
  members; several declarators must deduce one type.
- **Constrained block variables** (#286): `C auto x = expression;` uses a prior bare unqualified
  type concept. Sema evaluates the constraint on the invented parameter before completing the
  variable type, including for cv- and reference-qualified declarators. Namespace declarations,
  other initializer forms, template contexts, qualified or argument-bearing concept names,
  constrained `decltype(auto)`, and abbreviated function templates remain open.
- **Array structured bindings** ([dcl.struct.bind], #285): a block-scope declaration evaluates
  its initializer once and binds the names to elements or whole rows of the hidden array.
  Value bindings copy scalar elements in order. Spec mode takes cv from the declaration
  (CWG 2506). GCC and Clang modes also retain the initializer's cv-qualification.
  Reference bindings deduce and bind to the initializer with ordinary reference rules.
  `decltype(x)` gives the element type, and `decltype((x))` gives its lvalue reference.
- **Class-member structured bindings** ([dcl.struct.bind], #285): a block-scope declaration
  initializes a hidden object or reference once, then binds the names to public non-static data
  members in declaration order. The class is a non-template, non-union class with no bases,
  virtual layout, or bit-fields.
  Array and class members each take one binding. A reference member keeps its declared reference
  type in `decltype(x)` and aliases its referent, including through a const hidden object.
  Reference bindings extend a temporary's lifetime, including through a comma's right operand.
  Conditional and array-subscript paths to a temporary need lifetime lowering (#157).
  A complete `std::tuple_size<E>` with a member named `value` selects the tuple-like protocol,
  which is diagnosed as unsupported.
- **Deduced return types:** each `return` deduces and the first fixes the type for the rest of
  the body ([dcl.spec.auto.general]/11); no return deduces `void`. `BodySema` carries the deduced
  function type (`deduced`, `deduced_type`). Naming the function (`Sema::deduced_function`) asks
  `Questions::deduced_type`: a definition before the point, or a function template or class
  template member specialization instantiated where it is named ([temp.inst]/8); its own body
  sees the type once a return deduced it. The back end and the interpreter get the deduced type
  with the body. Mangling keeps a template's declared placeholder as `Da`/`Dc`.
  An unevaluated use of a non-constexpr static function of a class template specialization
  checks the member and its required callees when deducing the return type. An evaluated call
  or an emitted function address also demands a native definition. [#1272](https://github.com/yrashk/ycxx/issues/1272)
- **Linux direct calls:** the AMD64 ABI check uses the callee definition's deduced signature
  when the declaration still needs its result type. The definition is already in the translation
  unit. Calls can return supported `auto` and `decltype(auto)` results, including references and
  pointers. [#441](https://github.com/yrashk/ycxx/issues/441)
- **Calls through pointers to functions:** irgen emits `call_ind` (`Gen::call_pointer`), which
  lowering, AArch64 selection and the interpreter already had; constant evaluation calls through
  them too.
- Tests: `tests/own/auto/` covers array and class-member bindings, including hidden-object copies,
  aliases, cv-qualification, and `decltype` with reference and array members.
  `p1-impl-mangle` checks the `Da`/`Dc` names against Clang objects.
  `tests/own/structured-bindings/` adds class-member fixtures with Linux native execution.
- Not yet: constrained `C auto`, `auto x = {…}`, `auto x(e)` at namespace scope, abbreviated
  function templates (#286); tuple-like and remaining class-member bindings and other declaration
  contexts (#285), binding packs (#279), class member constant evaluation (#25), and mutable
  data members (#373).

## Lambdas, θ step 2 (#280, 2026-11-19)

The closure-type design is [docs/design/closures.md](docs/design/closures.md) (decided by Fable,
checked against Clang 23 and GCC 16 probes).

- **Closure types:** unnamed `class_` entities whose scope is the enclosing function (or the
  enclosing lambda's `operator()`), with the call operator's declared type and an ordinal per
  signature in closing-brace order (`EntityRec::ordinal`, mixed into the identity through
  `head`). Each is complete once its lambda's body ends, when its `ClassRec` is published with
  the implicit special members `special_members` gives (non-trivial copies and destructors of
  captures included). A capturing closure's default constructor is deleted.
- **Analysis** (`Sema::open_lambda`, `close_lambda`, the `LambdaFrame` stack): the body is
  parsed inside the enclosing part with its own locals; a name that resolves to an enclosing
  function's local or an init-capture is captured by every lambda between, by copy or by
  reference, explicitly or by the capture-default, outermost first; a constant usable in
  constant expressions is its value, not a capture. `this` and `*this`, init-captures, `mutable`,
  trailing return types and deduced return types (step 1). Captures are `Member`s of `This` in
  the body, and the lambda-expression is a `Lambda` node of `MemberInit`s. `BodySema::closures`
  keeps each `ClosureDef`.
- **Calls:** a class object's `operator()` is found and called ([over.call.object]); a member
  function called on a prvalue object materializes it. A captureless closure converts to a
  pointer to function through its static invoker (assignment, arguments, unary `+`).
- **Emission and ABI:** the front end emits each closure's `operator()` and invoker with the
  body that has it. Mangling: `<local-name>`s with `Ul <lambda-sig> E [n] _`, the invoker
  `8__invoke`, closure types as template arguments; a specialization for a TU-local closure is
  TU-local. Names and capture layout are checked against Clang objects (`p2-impl-mangle`).
- Tests: `tests/own/lambda/` (56 spec-only, 1 ABI): 55 pass, 2 XFAIL (default arguments #300,
  block-scope statics #291).
- Not yet: further lambda declaration contexts ([#338](https://github.com/yrashk/ycxx/issues/338));
  the conversion function's own symbol, conversions in
  contextual and operator contexts, invokers for class or variadic parameters (#339); arrays of
  non-trivially-copyable classes captured by copy (#353); weak definitions of closure members in
  inline functions (#124). A constant named by a lambda with a capture-default is captured, where
  Clang captures it only when it is odr-used (a layout difference for such closures, #355).

## Dependent exception specifications of templates (#828, #819)

A function template's noexcept-specifier, and that of an ordinary member function of a class
template, is part of each specialization's type. Bare `noexcept` and a condition that does not
depend on the template's parameters apply at once. A dependent condition is kept as tokens and
replayed for a specialization when it is needed: by the unary `noexcept` operator, or when the
specialization's address is taken ([except.spec]/13, [temp.inst]). This uses the replay that
member templates already had (#713, `replay_member_exception`); an explicit specialization keeps
its own specification. An invalid condition of a specialization that is never needed is no error.
In a template, a non-dependent `noexcept` on a pointer's function type is part of the type.
In a template's definition, `noexcept` on a call of such a function is value-dependent.
- A function template's exception specification can combine dependent `__is_nothrow_constructible`
  and `__is_nothrow_assignable` with `&&`, as in libycxx's move-swap overload in
  `include/ycxx/core/concepts.hpp`. After overload selection, replay forms both written type-ids
  with the selected arguments and checks class completeness at the demand. A failing substitution
  in that specification is a hard error. A nested requirement's replay skips semantic analysis of
  discarded operands that were parsed successfully in the template definition, including logical
  expressions in complete parenthesized groups. An enclosing equality, negation, call, cast,
  conditional, assignment or comma keeps its expression atomic
  ([#1314](https://github.com/yrashk/ycxx/issues/1314)). The trait evaluators cover the arithmetic
  cases and the written exact move operations above. Broader class queries remain in
  [#218](https://github.com/yrashk/ycxx/issues/218), with implicit and defaulted special-member facts in
  [#1303](https://github.com/yrashk/ycxx/issues/1303).
- Tests: `tests/own/template-noexcept/`.
- Not yet: the resolved specification in `decltype` of a template-id, in deduction from one, and
  in the address of a class template member (#838).

## A lambda's noexcept-specifier (#807)

A lambda's `noexcept`, `noexcept(true)` or `noexcept(false)` is part of its function call
operator's type ([expr.prim.lambda.closure]/4). The condition is in the scope of the lambda's
parameters and may not narrow to `bool` ([except.spec]/2). In a template's definition, a
condition that depends on the template's parameters makes the unary `noexcept` operator on a call
of the lambda value-dependent; each instantiation evaluates it with its own arguments. A
captureless non-throwing lambda converts to a pointer to noexcept function, and from it to a
pointer to function ([expr.prim.lambda.closure]/8, [conv.fctptr]).
- The init-captures are in scope in the lambda-declarator, as the parameters are
  ([basic.scope.lambda]): in the condition and in a trailing return type.
- Tests: `tests/own/lambda-noexcept/` (one run test, two reject tests).
- Not yet: a generic lambda's condition naming its own template parameters is rejected (#820).

## Lambdas in declarations (#338)

A lambda can be in the initializer of a namespace-scope variable, a static data member (of a class
or a class template), or a variable template, and in a constraint-expression. The design is
[docs/design/closures.md](docs/design/closures.md), "Closures of declarations".

- **Owner:** `Sema::ClosureOwner` gives the closure types of a declaration part their scope and
  linkage. A variable's is its entity, whose identity needs no type, so a placeholder's
  initializer has it too. A constraint's is the class or namespace around it, TU-local, with a
  discriminator: each satisfaction (`AtomAnalysis`) has its own closure types.
- **Generic lambdas:** `GenericClosure::part` says which declaration part a generic lambda is in.
  Its specializations are analysed in that part, which may still be being analysed.
- **Records:** `ItemSema` keeps a part's `ClosureDef`s. `Program::declaration_closures_` finds one
  by member for constant evaluation, and `Program::member_closures` keeps those of class templates'
  static members, which are instantiated when they are used.
- **Emission:** the closure's members have code where something refers to them (most are called
  in constant expressions only). They are weak when the owner is an inline variable, a static data
  member or a variable template's specialization with external linkage, and local otherwise.
  Names use the `<closure-prefix>`, as Clang 23 has them.
- **Constant evaluation:** closure objects are records the interpreter evaluates, with captures
  whose fields it represents, so constexpr lambdas work in function bodies too (#262).
- A lambda that is not local to a block has no capture-default ([expr.prim.lambda.capture]/3). Use
  of a variable of placeholder type in its own lambda is diagnosed.
- Tests: `tests/own/lambda-declarations/` (Clang 23 and GCC 16 agree; one ABI test against Clang
  objects), `unit.darwin_lambda_declarations`, `unit.linux_lambda_declarations`, and
  `unit.linux_namespace_lambda` (daemon edits with a cold comparison).
- Not yet ([#338](https://github.com/yrashk/ycxx/issues/338)): default member initializers, default
  arguments and `static_assert`. Unevaluated operands are the next section (#1549). Captures by reference in constant evaluation
  (#262), and weak definitions of variable
  template specializations, closure objects that are class templates' members and static locals
  of closures (#124) wait on their issues.

## Lambdas in unevaluated operands of declarations (#1549)

A lambda can be in an unevaluated operand (`decltype([] { … }())`, `sizeof`) of a declaration that
no variable owns: an alias-declaration, a member alias of a class or class template, an alias
template, and a function's declarator ([expr.prim.lambda.general]/3). libycxx's
`iterator_traits` uses it (`iterator_core.hpp`, `iterator_adaptors.hpp`).

- **Owner:** `Sema::unevaluated_closure_owner`, used by `open_lambda` where no function, enclosing
  lambda or `closure_owner` owns the closure: the class around it (`current_class`) or the item's
  namespace (`Questions::enclosing_namespace`), TU-local, with a discriminator of the item and the
  template arguments bound. A class template specialization's member alias has its own closure
  types, as its type-id is read again for it.
- **Alias templates:** one whose type-id makes closures, or names an alias template that does
  (`AliasCallPattern::closures`), is read again for each specialization, so `A<int>` and `A<char>`
  have different closure types ([temp.alias]/5). The same arguments name one type, as in GCC;
  Clang gives each use its own (#1593, `docs/compatibility.toml`). The read is a declaration part:
  its generic lambdas are instantiated in it, and `Program::alias_closures` keeps its closures,
  each once, for constant evaluation and emission where they are referred to. With dependent
  arguments its closures are patterns (`Sema::closure_patterns`).
- **Names:** such a closure is `$_N` in its class's or namespace's `<nested-name>`
  (`ClassName::closure_in_scope`), N unique in the unit, so a closure type can be a template
  argument and its members have code; in a local class, inside the function's `<local-name>`. A
  function or variable template specialization naming one is TU-local (`names_tu_local`). Clang
  and GCC give a class template's member closures vague linkage and per-class `Ul…E` names
  (#1593). A constraint's closures keep their structural names.
- Tests: `tests/own/lambda-unevaluated/` (Clang 23 and GCC 16 agree; also run on x86-64 Linux in
  the `gcc:16` image), and two units naming each its own closure type in
  `unit.{darwin,linux}_lambda_declarations`.
- Not yet: a lambda in a function template's signature, which stays Dependent like any dependent
  `decltype` there (#1269; an XFAIL test), or in a default template argument (#1608). An
  out-of-line definition with a lambda in its return type matches its declaration (#1609).

## Floating point, the μ slice (#336, 2026-11-19)

`float`, `double` and `long double` (binary32, binary64 and binary64 on arm64 Darwin) have
values, end to end; results match Clang's bit for bit.

- **Literals** ([lex.fcon]): decimal and hexadecimal, rounded to nearest by the lexer (`l`'s in
  binary64 too); one beyond the type's range is an infinity with a warning, as [lex.fcon]/4's
  example and Clang and GCC have it. The `f16`/`bf16` suffixes and the extended types are #351.
- **Constants:** `ast::VFloat{type, bits}`, the encoding in the type's format; `ast::float_value`,
  `float_double` and `computes(format)` in `ycxx.ast.typeops`; `sema::ValueRec::real`.
- **Sema:** `is_arithmetic` includes the standard floating types; `ConversionKind::floating` for
  [conv.fpprom] and [conv.double] beside `floating_integral` ([conv.fpint]), chosen by
  `Sema::arithmetic_conversion` wherever an arithmetic conversion is made; the usual arithmetic
  conversions, overload ranks (float to double a promotion), the ellipsis's float-to-double
  promotion, narrowing ([dcl.init.list]/7.1–7.3, with constant values), `static_cast` between
  floating, integer and enumeration types; an enumeration with a floating operand is ill-formed
  ([expr.arith.conv]/1); `%`, shifts, `~` and subscripts reject floating operands.
- **Constant evaluation:** the interpreter computes in binary32 and binary64 (`float_arith`,
  `float_compare`, `float_convert`). In constant mode a division by zero, a NaN result and a
  conversion to an integer that cannot hold the value are not constant in spec and Clang modes.
  Overflow to infinity from finite operands is constant in these modes. GCC mode permits an
  existing NaN operand to propagate and rejects overflow from finite operands.
- **L1/L2:** L1's `fadd`…`fcmp`, `fconv`, `f2i`, `i2f` and the new `fabs`; L2 gains `f32`, `f64`,
  `fadd`, `fsub`, `fmul`, `fdiv`, `fneg`, `fabs`, `fcmp` (plain predicates: ordered but `ne`),
  `fpext`, `fptrunc`, `fptosi`, `fptoui`, `sitofp`, `uitofp`. `lower` extends a narrow integer to
  32 bits before converting it, converts to bool by `fcmp ne 0`, and splits a homogeneous
  floating-point aggregate (`TypeLayout::hfa`, 1–4 members of one floating type, through arrays,
  member classes and unions) into one value per member.
- **AArch64:** `R::v0`–`v31`; the FP forms (C7.2) with their own size rules (`Size::ftype`,
  `sfft`); the pattern conditions `f32(?x)`, `f64(?x)` and `?d` for the root; FCMP's plain
  predicates map to conditions false for unordered operands (`lt` is MI, `le` LS). Floating
  arguments take v0–v7 counted apart from x0–x7, then the stack in argument order; results come
  back in v0 (an HFA's in v0–v3); an HFA that does not fit goes whole on the stack (AAPCS64 C.3);
  variadic doubles take 8-byte stack slots. The slot allocator passes floating values through
  v16–v19; frame layout gives a form its FP counterpart when its register is a V register
  (loads, stores, CSEL, moves, constants built in x16 and moved by FMOV). The internal
  disassembler names S and D registers.
- **Built-ins** (docs/EXTENSIONS.md): `__builtin_fabs*` (`ast::BuiltinCall`, FABS),
  `__builtin_inf*`, `__builtin_huge_val*` and `__builtin_nan*` (constants). `__has_builtin`
  reports the `long double` forms on targets with a supported value format.
- On Linux x86-64, a dependent `__builtin_fabsl` call in a template definition keeps its
  `long double` result type. When instantiation selects the call and the operand is concrete,
  a well-formed call receives the unsupported x87 value diagnostic ([stmt.if], [temp.inst], #201).
- `__builtin_nansl("")` is recognized as a `long double` prvalue in libycxx's `<limits>`
  template, including when the arm is discarded or the expression is unevaluated. A reached
  constant evaluation or native emission diagnoses the unsupported signaling NaN value.
  `__has_builtin` answers 0 on all targets. x87 signaling NaN values remain in #645, and other
  GNU builtin payloads remain in #248. [#1397](https://github.com/yrashk/ycxx/issues/1397)
- `#include <cmath>`: 4,415 errors before, 4,343 after; it still does not compile (#349).
- Tests: `tests/own/floating` (84 spec-only, and 4 from the review: literals, conversions, constant evaluation, run
  tests printing with `printf`, mixed-object tests with Clang peers for scalars, many arguments,
  HFAs, structs and variadic callees, and `<math.h>`); 11 are XFAIL on other features (#25, #94,
  #117/#349, #196, #267, #291, #298, #307). Breaking HFA passing or the `lt` condition turns
  them red. `unit.aarch64`: the FP encodings against the Arm ARM. Five draft examples pass.
- Not yet: the extended floating-point types (#351), floating non-type template parameters
  (#317), `-ffast-math`-style options, and `<cmath>` (#349).

## Generic lambdas, θ step 3 (#281, 2026-11-19)

- **Definition:** `auto` parameters invent type parameters appended to the lambda's
  template-head, after any explicit `[]<class T>` ones ([expr.prim.lambda.closure]/7); the head
  is pushed onto the template scopes, so the body is analysed as a template's definition, and the
  call operator keeps its declared type. Its record's `MemberDeclRec::head` makes it a template;
  `ClosureDef` keeps the head and the enclosing function's constants the body names
  (`consts`). The closure's name has the head at depth 0 and Clang's `Ty`/`Tn` declarations for
  explicit parameters (`EntityRec::closure_head`).
- **Calls:** `member_call` deduces each template operator's arguments (`Templates::deduce_call`)
  and asks `Questions::generic_instance` for the specialization, which `Program` instantiates
  where it is named ([temp.inst]/8): a `BodyAnalysis` in generic mode re-analyses the lambda's
  body tokens with the arguments bound and the closure's frame rebuilt from its record
  (`Sema::open_generic`, `close_generic`), deducing the return type. Generic lambdas defined in
  the part being analysed are found through `generic_defined`; ones nested in another generic
  lambda through that lambda's instance. A recursive call through a `self` parameter sees the
  type its first return deduced. The instances are `Program::instances()`, emitted as the
  others, mangled as member template specializations in a <local-name>.
- **Constant evaluation of closures:** `Questions::function_ir` of a closure member is answered
  (`Evaluator::closure_callee` for the part being analysed, `Program::closure_ir` otherwise),
  and closure members are constexpr. The interpreter evaluates closure objects (#338); captures by
  reference wait on it (#262).
- Tests: `tests/own/lambda/p3-*` (33 spec-only + 6 own, one an ABI test against Clang objects):
  all but 6 pass; those are XFAIL (default arguments #300,
  dependent `decltype` #199, a generic lambda's conversion #339, explicit
  template arguments #347).
- **Packs:** `auto... xs` invents a template parameter pack and `[]<class... Ts>(Ts... xs)`
  declares one; a specialization's parameters are the pack's elements (`open_generic`), and the
  body's expansions are those its definition recorded (`GenericClosure::expansions`, kept in the
  enclosing body's `BodySema`). `[xs...]` and `[&xs...]` capture each element of an
  instantiation's function parameter pack, as a capture-default does for the elements used.
- Not yet: init-capture packs `[...ys = xs]`, requires-clauses, static and consteval lambdas
  (#350).

## Throw expressions (#551)

A throw-expression has type `void`, keeps its operand in the semantic tree, and
supports rethrow syntax. Scalar and pointer operands are checked after the applicable
decay conversions. Conditional expressions preserve the other arm's type and value
category when one arm is a throw-expression. Class exception construction, constant
evaluation of a reached throw, and native rethrow have explicit diagnostics.

On Linux x86-64, a certified cleanup-free ordinary function can throw an `int` to a
hosted peer's handler. The operand is evaluated once before allocation. The generated
object references the peer's Itanium exception allocator, throw entry point, and
`int` type information. Other exception types, native handlers, exceptional cleanup,
and runtime integration with ycxx's own linker remain under [#89]. See the
[design](docs/design/linux-int-throw.md) and [#578](https://github.com/yrashk/ycxx/issues/578).

[#89]: https://github.com/yrashk/ycxx/issues/89

## Statements, ι step 1: `switch`, labels, `goto`, conditions (#337, 2026-11-19)

- **Parser and sema** (src/parse/parser.cppm): `switch`, `case`, `default`, identifier labels and
  `goto` ([stmt.switch], [stmt.label], [stmt.goto]); a label may end a compound statement (C++23).
  Case values are converted constant expressions of the promoted condition type, checked for
  narrowing and duplicates. A condition may declare a variable (`CondDecl`, [stmt.pre]/4) in `if`,
  `if constexpr`, `while`, `for` and `switch`. An `if` or `switch` init-statement can also use an
  alias-declaration (C++23). For `if constexpr` (#344), the converted condition must be constant,
  and initialization of the decision variable precedes its use in the selected arm.
  `Sema::initialized_in_scope` lists the variables whose initialization is not vacuous;
  a `goto` or a case label that would enter their scope is an error
  ([stmt.dcl]/2), and labels are checked when the body ends (`Sema::check_labels`). A substatement
  of `if constexpr` or `if consteval` is a `jump_barrier`, which no jump may enter ([stmt.if]/2, /4).
- **IR** (src/irgen/gen.cppm): a switch is a chain of `icmp`/`cond_br` over its case values found
  in the body first (`case_labels`), since L1's `switch_` op has no lowering yet (#342). A `goto`
  ends the objects of the scopes it leaves and those begun after a label it jumps back to
  ([stmt.jump]/2); a `goto` before its label branches to a block filled in when the label is
  placed. Jump and switch dispatch edges that bypass a vacuous declaration allocate storage
  and start its lifetime ([stmt.dcl]/2, #341). The active object's address is kept in a
  function-scope binding so fallthrough and jumps share uses and scope cleanup at the label.
  A condition's variable lives one iteration of a loop, through a `for`'s increment. A
  goto's `br` carries `not_constexpr`, so constant evaluation of it fails ([expr.const]).
- Not yet: a jump past a vacuously initialized class with a nontrivial destructor (#477),
  `switch` on a class with a conversion function, and `explicit operator bool` conditions (#339).
- Tests: `tests/own/stmt/` (written spec-only, 23 run and 15 reject tests, and the split-off XFAILs
  for the above).

## Default arguments, ι step 2 (#300, 2026-11-19)

- **Declarations** (src/parse/parser.cppm `default_argument`, parse.cppm `merge_defaults`): a
  default argument is analysed where it is written ([dcl.fct.default]/5) and kept, converted to the
  parameter's type, as a `DefaultArg` tree in `ParamSema::default_arg`, the slot ast.md reserved.
  Redeclarations at namespace scope inherit earlier defaults (`b_.adopt`), may not repeat one, and
  leave none missing after one ([dcl.fct.default]/4). Ordinary member defaults written in a
  non-template class are replayed in the complete-class context ([class.mem.general]) and kept
  in `MemberDeclRec::defaults` (#354).
- **Calls** (src/sema/actions.cppm): `Candidate::defaults` and `required` make a candidate viable
  with fewer arguments ([over.match.viable]/2); `Templates::deduce_call` skips defaulted trailing
  parameters. `call_args` and `construct` adopt the chosen function's trees (`default_argument`),
  so a constructor whose parameters all have defaults is a default constructor, and one with one
  required parameter converts. `Sema::defaults_` records what lookup found per function.
- **IR:** a `DefaultArg`'s nodes take the call's position (`Gen::pinned`), since its offsets are
  its declaration's.
- Each call takes the defaults its own lookup found (`Sema::defaults_` by DeclRef node), since
  declarations in different scopes have their own. A copy or move constructor's other parameters
  may have defaults. Defaults are rejected where [dcl.fct.default]/3–4 and [over.oper.general]
  forbid them (`defaults_allowed`, `defaults_trail`).
- Omitted defaults of function templates replay with the selected bindings at the written
  declaration, for any parameter type. Explicit arguments bypass replay.
- **Class templates (#356):** member defaults wait for the complete class in the template's
  definition and in each specialization (the `member_template_name` gate in `ItemAnalysis`).
  So do a templated class's member templates' defaults. An ordinary class's member templates'
  defaults bind at their declaration, as its exception specifications do (#1607). Class
  completion binds each default on first need (`bind_destructor_and_defaults`), so a default
  that uses itself or a later one is diagnosed (`default_arg_recursive`,
  `default_arg_not_yet_defined`). A call made before then says the same (`defaults_pending`).
  The definition checks defaults unbound (`bind_member_default` with the enclosing heads). A specialization keeps each as a pending `DefaultArg` marker
  (`Program::pend_member_defaults`). `AtItem::replay_member_default` analyses it for the call that
  uses it ([temp.inst]/12), with the class's arguments bound and the definition's dependent calls
  completed by argument-dependent lookup at the point of instantiation. Member function templates
  replay in `replay_function_default` with the class's arguments at the enclosing levels. In a
  template's definition, a call that selects a dependent default is value-dependent (`call`).
  A specialization does not replay a default its definition failed to analyse. Nested
  replays count toward the instantiation limit ([temp.inst]/16).
- Not yet: lambdas in deferred member defaults (#338); a nested class's defaults naming later
  members of the enclosing class (#1602); later members in an ordinary class's member template
  defaults (#1607); defaults added by an out-of-class
  or qualified definition (#354); defaults added after a using-declaration (#362, the oracles disagree).
- Tests: `tests/own/default-args/` (spec-only, with XFAILs for #354, #357 and #102), including
  `class-template-*` and `function-template-*` for #356.

## Dynamic initialization of namespace-scope variables, ι (#94, #337, 2026-11-19)

- **Front** (src/parse/parse.cppm): a namespace-scope variable whose initializer is not a constant
  expression keeps its initializer tree instead of an error (constexpr and constinit ones must
  still be constant); `T x(args)` and default-initialization by a constructor (an object or an
  array of them) are dynamic.
- **The unit's initialization function** (src/pipeline/front.cppm `init_function`): one
  `StaticInit` statement per such variable, in definition order ([basic.start.dynamic]/1), each
  initializing the object from its declaration's tree (positions pinned, as for default
  arguments) and registering the destructor with `__cxa_atexit(d, &object, &__dso_handle)`
  ([basic.start.term]; each array element on its own). It has internal linkage and Clang's name,
  `__GLOBAL__sub_I_<file>`; a pointer to it goes in `__DATA,__mod_init_func`
  (`obj::standard_section::mod_init_func`, `aarch64::DataKind::init_func`), which dyld runs before
  `main`. A dynamically initialized `const` variable is placed in writable data.
- An initializer is dynamic when it is not a constant expression but the front end has code for
  it (`sema::has_runtime_code`); one it has no code for, or a scalar's that hit the evaluator's
  limits, is still an error. Class objects the interpreter cannot evaluate yet are initialized
  dynamically even when constant by the draft (#25). Each declaration's variables get their own
  function (`__GLOBAL__sub_I_<file>.<n>`), so an error points at it.
- Namespace-scope references to supported scalar and array storage use a pointer-sized slot
  ([dcl.ref], [dcl.init.ref]). A constant binding uses a symbol-plus-addend relocation and
  exposes the referent's address in constant evaluation ([expr.const.init]). A dynamic binding
  stores the selected address at the declaration's position in the unit's initialization order.
  Later reads, writes and address-taking use that address. An `extern` declaration without an
  initializer reserves no storage. See [the design](docs/design/namespace-references.md) (#367).
- Remaining reference cases include function bindings, lifetime extension, guarded statics and
  class or base subobject addresses outside the constant evaluator's model (#25, #367).
- Not yet: inline variables and variable templates (#368);
  remaining nonconstant default member initializer forms (#118); array construction as a loop (#370).
- Tests: `tests/own/dynamic-init/` (spec-only), including a Clang peer with its own initializers
  in `peer.run.cpp`. The Linux executable test runs the scalar reference fixtures with internal
  linkage and checks their ELF relocations.

## Nested namespace definitions, ι (#153, #337, 2026-11-19)

- `namespace a::b::c { }` (C++17) and `namespace a::inline b { }` (C++20), [namespace.def]: the
  item splitter declares every component (`namespace_names`), the first in the enclosing namespace
  and each later one in the one before it (`Program::name_scopes_`, read by `declared_in`); only a
  component preceded by `inline` is transparent to lookup. `namespace_definition` gives each
  component its declaration. Names mangle as Clang's (`tests/own/namespaces/nested-mangling`, with a
  Clang peer).
- Tests: `tests/own/namespaces/`.

## Block-scope static variables, ι (#291, #337, 2026-11-19)

- **Sema** (src/parse/parser.cppm `variable_node`, src/sema/actions.cppm `static_local`): a
  `static` (or `constinit static`) block-scope variable is an entity local to its function or
  lambda, numbered among its statics of that name (`EntityRec::ordinal`, also in its identity),
  declared as a `StaticVar` node with the constant it is initialized with when there is one.
- **Pipeline** (src/pipeline/front.cppm `static_locals`): each is a variable of the unit, with
  its constant or zero; one initialized by code, or with a destructor, gets a guard variable
  (8 bytes) and its function the runtime's entry points (`Runtime`). Names are Clang's: the
  function's own symbol gives the <encoding> of `_ZZ<encoding>E<name>[_n]` and `_ZGVZ…` (back.cppm).
  libycxx-abi provides them in a default Darwin link (#842); the Mach-O linker adds libc++abi
  (`libcxxabi()`) for names that remain and only it exports, as with `-nostdlib++`.
- **IR** (src/irgen/gen.cppm `static_var`): `if (__cxa_guard_acquire(&g)) { init; at_exit(d);
  __cxa_guard_release(&g); }`, the acquire called on every pass (#372); a static local's object is
  its entity's from the function's entry, so a jump past its declaration finds it.
- Not yet: inline functions' statics are per unit (#124); static references (#367).
- Tests: `tests/own/static-locals/` (spec-only, 19 tests, a Clang peer in `mixed.run.cpp`).

## Named `int` bit-fields (#307, 2026-11-19)

- Ordinary unpacked, non-template classes without bases support named non-static `int` and
  `unsigned int` bit-fields of width 1 through 32 ([class.bit]). The AST retains the width
  in the declaration and member expressions. Sema and the back end share the target byte
  and bit placement, including a signed 24-bit member followed by an ordinary byte.
- In the native pipeline, field projections carry the record pointer and field index. Loads
  sign-extend signed values and stores mask only the field's occupied bytes. Static aggregate
  initialization uses the same layout. Width-aware integer promotions apply in built-in
  arithmetic, shifts, and the other promotion contexts ([conv.prom]).
- Remaining scope in #307 includes other declared types, unnamed fields, union variants,
  templates, packed layout, default member initializers, reference binding, volatile access,
  bit-field conditional glvalues, and constant evaluation of bit-field access. The preprocessor
  still passes `#pragma pack` through for `-E`.
- Tests cover widths and alignments on both targets, hard diagnostics, static data bytes and a
  width edit in `unit.ast_types`, `unit.sema` and `unit.pipeline_incremental`.

## Range-based `for`, ι (#298, #337, 2026-11-19)

- **Parser** (src/parse/parser.cppm `range_for`): `for ( init-statement_opt decl : initializer )`
  (`range_for_ahead` finds the `:`, past `?:`) is built as [stmt.ranged] expands it, from existing
  kinds: hidden locals `auto&& __range`, `auto __begin`, `auto __end` (`deduce_placeholder`),
  then a `For` over `__begin != __end` and `++__begin` whose body declares the loop variable from
  `*__begin`. An array supplies its begin and one-past-end pointers. A class supplies member calls
  when member lookup finds both names, including inherited members. The fallback finds
  `begin(__range)` and `end(__range)` through ADL alone (#165). The loop
  variable and init-statement names are in the guarded scope ([basic.scope.block]/2). A dependent
  range is expanded at instantiation.
- Remaining work covers C++23 lifetime extension for temporaries inside the range initializer
  (#376), braced ranges (#329), and sentinels that rely on
  `!=` rewritten from `==` (#295). A constexpr reference requires a constexpr overloaded
  dereference. Full constant evaluation of the class iterator still needs #25.
- Tests: `tests/own/range-for/` (17 from the independent author and nine review regressions).

## Concrete exception specifications on ordinary function types (#219)

- Ordinary non-template declarations, aliases, and grouped declarators retain concrete
  exception specifications through pointers, references, and return deduction.
- Overload-address selection, reference binding, and function-pointer conversions permit
  a nonthrowing function to match the corresponding potentially throwing type.
  The reverse direction and conversions through multiple pointer levels are rejected.
- Composite pointer types preserve the one-way rule in comparisons and conditional
  expressions. Function reference overload ranking follows [over.ics.ref].
- The mixed Linux object checks distinguish callback overloads with the Itanium `Do` type
  encoding. Dependent exception specifications remain tracked in [#219](https://github.com/yrashk/ycxx/issues/219).

## Unary noexcept on calls and constructions (#729)

`noexcept(expression)` is a constant `bool` and leaves its operand unevaluated
([expr.unary.noexcept]). The compiler checks the selected function or constructor,
argument operations, and temporary destruction ([except.spec]). A construction
in a function template body can be checked after substitution.
For one-dimensional scalar `new T[n]`, the operator accounts for the selected
global array allocation function along with the bound and initialization.
[#854](https://github.com/yrashk/ycxx/issues/854)

## Exception specifications on member function templates (#713, #804)

A selected ordinary member function template can instantiate its written
`noexcept(expression)` with the class and function template arguments
([except.spec], [temp.inst]). Its parameter and class scopes are available
during replay. Class instantiation leaves the specification of an unused
member template deferred.

Selected member function template specializations preserve bare `noexcept`
for calls and for selection against a static function-pointer target.

## The address of an overload set, ι (#313, #337, 2026-11-19)

- A pointer or reference target picks a free or static member function by its function type
  ([over.over]). Targets include initializers, assignments, arguments and return values, and
  casts. List-initialization applies the target to each scalar element, including elements of
  an array with a deduced bound.
- Function template deduction uses the entire target type ([temp.deduct.funcaddr]). Selection
  checks associated constraints, prefers a non-template function, and partially orders
  matching templates by their full function types. A call can also deduce fixed function or
  function-pointer parameters by trying the non-template members of an overload set
  ([temp.deduct.call]/6).
- Related work covers pointers to non-static members (#238), dependent exception specifications (#219),
  a template-id without a target type (#335), and function pointer or reference template
  arguments (#406).
- An ordinary free immediate function is diagnosed when its name escapes through a runtime
  pointer, reference argument, or constructor argument. The same check applies after overload
  selection. It preserves immediate invocations through a name or parenthesized name,
  unevaluated operands, and manifestly constant-evaluated initializers and assertions. GCC and
  Clang modes diagnose constexpr initializers whose result retains the pointer or reference.
  An immediate invocation may still consume it (#102).
- The Linux x86-64 backend supports calling through the address of an ordinary function with
  internal or external linkage. [#407](https://github.com/yrashk/ycxx/issues/407)
- Tests are in `tests/own/function-pointers/`, with draft examples for [over.over] and
  [temp.deduct.call] enabled.

## Member using-declarations of base member functions (#1150, 2026-11-26)

- `using B::f;`, `using B::operator();` and `using B::operator T;` in a class's
  member-specification ([namespace.udecl]) name member functions of a base class, direct or
  indirect, of an ordinary class, a class template or a partial specialization
  (`member_using_declarator` in parse.cppm). A dependent base (`using Fn::operator();`) is named
  again by each specialization's analysis of the member item. What lookup in the base finds is
  kept in `Program::member_usings_` by (member item, class entity).
- `Program::introduce_members` puts them in `ClassRec::introduced` once the class's own members are
  known. It leaves out those that one of the class's own member functions hides or overrides: the
  same name, parameter-type-list, cv-qualifier and ref-qualifier (/11). A conversion function also
  compares its conversion type. A member function template also compares its template-head's
  parameter kinds and depths (a class template's member template never hides an ordinary base's:
  #1437). A constrained declaration hides nothing. The using-declaration's access goes in
  `ClassRec::access` (/16). A data member or member type of the same name conflicts with it.
- In a template definition, a name taken from a dependent base (`using Fn::name;`) is
  type-dependent ([temp.dep.expr]/3; `Program::dependent_member_usings_`), so unqualified calls of
  it are resolved in each specialization. In an incomplete class (a static member's initializer, a
  `static_assert`), lookup also finds what earlier using-declarations introduced (`members_before`),
  and a static initializer's call ranks every earlier declaration of the name.
- Lookup (`direct_member`, `member_functions`) finds them as members of the class, so they join
  its overload set ([over.match.funcs.general]/4). A call selecting one converts the object to
  the declaring base subobject, whatever the base's access, and a virtual one dispatches through
  that base's table.
- A call of a class object finds `operator()` in a base too ([over.call.object]), through the base
  subobject unless every candidate is static.
- Checks: the nominated class is a base (/3), the member exists and each function is accessible
  (/14), and a declarator is not repeated in its class.
- Not yet: member types, data members and enumerators (#1425); inheriting constructors and pack
  expansions `using Fs::operator()...;` (#308). Conversions do not consider a class with bases
  (#339). Operator expressions do not find a base's member operators (#1426), and overloaded
  `operator[]` is not supported (#219). Access to an inherited injected-class-name is not checked (#1427).

## Pointer-to-member types and declarators (#238)

- Declarators build `TMemberPtr` ([dcl.mptr]), interning the owning class and member type as
  separate components. The owner can be incomplete, a union, or dependent. Member function
  types retain their cv-qualifiers, ref-qualifier and non-dependent exception specification.
  The member-pointer traits distinguish these from ordinary pointers.
- Substitution, deduction and pack tracking traverse both components ([temp.dep.type],
  [temp.deduct.type]). A requires-expression checks substitution into its local parameter
  declarations. The parameters have no storage and simple and compound requirements are
  unevaluated operands ([expr.prim.req.general], [expr.prim.req.simple], [expr.prim.req.compound]).
- The two libycxx `<cstdio>` requires-expressions using dependent member-pointer declarations
  can be checked. Linux hosted header support still depends on #395, #396 and #398.
- Object size and alignment follow Itanium C++ ABI §2.3 on the two supported targets.
- Tests in `tests/own/member-pointer/` cover declarations, type identity, dependent requirements
  and invalid pointer-to-member types.

## Ordinary data member-pointer expressions (#411)

- An unparenthesized `&C::field` forms a data member pointer for an accessible, directly declared
  non-static field of non-reference type. Bit-fields have no address. A null pointer constant
  converts to the typed null data member pointer; value-initialization also gives that value.
- Built-in `.*` and `->*` apply a data member pointer to an object of its declaring class. The
  result incorporates the object's cv-qualifiers. `.*` yields an lvalue when the object is an
  lvalue and an xvalue otherwise. `->*` yields an lvalue. `.*` participates in temporary lifetime
  extension ([class.temporary]).
- Constant expressions can retain a named field or typed null. Applications in constant
  expressions need interpreter support for class objects alongside ordinary class member access
  ([#25](https://github.com/yrashk/ycxx/issues/25), [#262](https://github.com/yrashk/ycxx/issues/262)).
- Tests are in `tests/own/member-pointer/`. Native representation, member-pointer conversions,
  equality, contextual bool and member function pointers are tracked in
  [#411](https://github.com/yrashk/ycxx/issues/411). Dependent exception specifications are tracked
  in [#219](https://github.com/yrashk/ycxx/issues/219).

## Comparisons of pointers to qualified arrays (#445)

- Equality and relational comparisons of pointers to complete arrays of the same shape combine
  the element cv-qualifiers in the composite pointer type ([expr.eq], [expr.rel], [expr.type],
  [conv.qual]). Nested array dimensions and comparisons between `const` and `volatile` arrays
  use the same rule.

## Composite types involving pointers to void (#59)

- Equality, ordering and conditional expressions combine a pointer to cv-qualified `void` and
  a pointer to an object or `void` into a pointer to `void` with the union of the pointee
  qualifiers ([expr.type], [expr.eq], [expr.rel], [expr.cond]).
- Converting a pointer to an array to a pointer to `void` preserves the array's element
  qualifiers ([basic.type.qualifier], [conv.ptr]).

## Ordinary scalar allocation and deallocation (#293)

- [expr.new] and [expr.delete] support complete, default-aligned integral and ordinary pointer
  objects. The result of `new` is a prvalue pointer to the allocated type, with its cv-qualifiers.
  Omitted initialization leaves the bytes alone, empty parentheses or braces give zero or null,
  and explicit initializers use the scalar conversion and list-narrowing rules.
- The unaligned global declarations of `operator new` and both `operator delete` overloads use the
  target's `sizeof` result type. Written declarations and definitions of those signatures share
  the same entities as implicit declarations. Allocation passes `sizeof(T)`, and single-object
  delete evaluates its operand once and passes its address and `sizeof(T)` to the sized global
  deallocator when non-null. The scoped spellings use the same functions. Global lookup and
  sized dispatch follow Astra's independent draft and Itanium ABI review on #293.
- The implicit global aligned scalar signatures are `operator new(size_t, std::align_val_t)`,
  `operator delete(void*, std::align_val_t) noexcept`, and
  `operator delete(void*, size_t, std::align_val_t) noexcept` ([basic.stc.dynamic]). Written
  redeclarations use the same scoped enum with the target's size type as its underlying type.
  Source lookup of the enum spelling requires a declaration. The aligned fixtures exercise typed
  allocation address selection, direct deallocation calls, and a Linux peer providing all three
  symbols. Over-aligned storage and expression dispatch are tracked in
  [#293](https://github.com/yrashk/ycxx/issues/293) and [#303](https://github.com/yrashk/ycxx/issues/303).
- Written nonthrowing global scalar and array allocation and deallocation overloads can take a
  final `const Tag&`, with an optional preceding `std::align_val_t`. The tag can be a named
  incomplete class or union, including `std::nothrow_t`. These declarations enter ordinary
  overload lookup when written and support explicit calls and typed addresses. A default-aligned
  `new` or `new[]` diagnoses ambiguity when defaults visible at the expression make a written
  overload and the usual size-only allocation function equally viable. Tagged placement
  expressions remain in [#293](https://github.com/yrashk/ycxx/issues/293).
- The six usual global array allocation and deallocation declarations support explicit
  calls and typed address selection, including the aligned signatures. Written
  redeclarations share the implicit entities. The Itanium mangler emits `na` and `da`, and
  Linux object tests call providers built with both GCC and Clang. Broader expression codegen and
  further allocation-function declarations remain in [#293](https://github.com/yrashk/ycxx/issues/293).
- The four C++26 non-allocating placement functions can be declared and defined at global scope
  ([new.delete.placement]). The two allocation functions take `size_t, void*` and return `void*`.
  The two deallocation functions take `void*, void*` and return `void`. These signatures require
  both arguments and a nonthrowing exception specification. Written `constexpr` definitions can
  be called explicitly in constant expressions.
- Runtime scalar placement `new` supports integral and ordinary pointer objects with one placement
  argument. Overload resolution must select a visible written global `operator new(size_t, void*)`
  with a nonthrowing exception specification. The call precedes scalar initialization in the returned
  storage. Native lowering requires a supported nonthrowing initializer. Constant evaluation of
  storage reuse, placement arrays and classes, and more general lifetime and cleanup rules remain in
  [#293](https://github.com/yrashk/ycxx/issues/293).
- Native lowering supports initializers proven nonthrowing from the analyzed expression tree.
  Initializers that need an exception cleanup edge and constant evaluation of allocation remain
  in #293. Namespace-scope allocation uses the existing dynamic initialization path. The issue
  also tracks class arrays, enumeration, floating-point, placement and over-aligned allocation,
  and other allocation-function declarations.
- Spec-only tests in `tests/own/scalar-allocation/` cover types, initialization, a typed null, and
  invalid operands. A separate provider checks the address and size passed to deallocation.
- Single objects of complete, default-aligned class type can be allocated and deleted on both
  targets, including in templates with a dependent type or initializer. `new` passes
  `sizeof(T)` to `operator new(size_t)` and initializes the object as a declaration would:
  default-initialization without an initializer, value-initialization for `()` (the storage is
  zero-filled first when that requires zero-initialization), and direct- or
  direct-list-initialization by the selected constructor or as an aggregate, including from a
  pack expansion. A const object needs a const-default-constructible class
  ([dcl.init.general]/8). Placement `new` constructs a class object the same way in storage
  from a visible written reserved `operator new(size_t, void*)`. `delete` evaluates
  its operand once and, when non-null, runs the destructor (none when trivial) and passes the
  address and `sizeof(T)` to `operator delete(void*, size_t)`. Deleting an incomplete class is
  ill-formed in C++26 and diagnosed. A constructor that throws does not yet release the storage
  ([expr.new]/28): the frame gets no unwind information, so the exception terminates the program
  ([#1135](https://github.com/yrashk/ycxx/issues/1135)). Polymorphic deletion through a virtual
  destructor ([#931](https://github.com/yrashk/ycxx/issues/931)), class-specific allocation
  functions, over-aligned classes, class arrays and a braced argument inside parentheses are
  diagnosed as not supported yet (#293).
  `tests/own/class-allocation/` runs on Darwin, and `unit.linux_class_new` runs the same
  sources on Linux x86-64 with a host-built replacement allocator.
- Syntax checking supports one-dimensional `new T[n]` and `delete[] p` for integral and ordinary
  pointer elements ([expr.new], [expr.delete]). The bound converts to `size_t`. A potentially
  evaluated constant bound can be zero and must keep the object size within the positive range
  of `ptrdiff_t`. The array AST retains default or empty initialization and the typed delete operand.
  On Linux x86-64, array deletion evaluates that operand once and passes the original address to
  the usual unsized `operator delete[](void*)` when non-null. The call releases the allocation,
  including zero-length arrays.
- Linux x86-64 can allocate these default-initialized arrays in ordinary functions certified
  for cleanup-free unwinding. The bound can use integer literals, parentheses, built-in unary
  `+` and `-`, and built-in binary `+`, `-`, and `*`, with integral types up to 64 bits.
  The usual global `operator new[](size_t)` receives the checked byte count, including zero.
  Lowering preserves the returned address and leaves the element bytes alone. A hosted caller
  can catch an exception from a replacement allocator across the wrapper.
  `unit.linux_fixed_array_new` checks call relocations, unwind frames, allocation, and release
  for both single literals and computed bounds. Named constants, dynamic bounds, and broader
  allocation and deletion remain in #293.
  Constant-evaluated allocation and deletion are tracked in #321.
  [Cookie-free deletion](docs/design/cookie-free-array-delete.md)
  A file-scope scalar initializer still needs lowered code to qualify for dynamic initialization.
  Type checking covers function-local expressions and unevaluated operands.

## Qualified template functional conversions (#697)

A namespace-qualified class or alias template-id can name the target of a parenthesized
or braced functional conversion. A namespace alias resolves to the same template, and defaults
can come from its visible redeclarations. Qualified lookup stops at the first matching namespace
on each using-directive path. Conflicting declarations across paths are ambiguous. Qualified
function and variable template expressions use the ordinary expression path
([expr.type.conv], [temp.names], [basic.lookup.qual]).
[#697](https://github.com/yrashk/ycxx/issues/697)

## Typed declaration annotations (#662)

Class heads and ordinary function declarations accept C++26 `[[= expression ]]` annotations
and annotation lists ([dcl.attr.annotation]). The compiler retains each occurrence's canonical
payload type, evaluated value and item-relative token offset. A translation-unit consumer can
query all written occurrences by class or function entity, including repeated values and
redeclarations, while overloads keep separate lists. Enum payloads retain their enumeration type.
Supported aggregate payloads use structural fields and trivial destruction. Reference-member and
union payload evaluation, the reflection operator, public `std::meta` APIs, template annotation
substitution and broader annotation targets remain in [#324](https://github.com/yrashk/ycxx/issues/324).

## Bootstrap workarounds

Workarounds for C++26 and module limitations in the bootstrap toolchain (DECISIONS §3.2).
Each is removed once ycxx compiles itself.

- **No `std::vector<bool>` in the back end's unit records** (`pipeline::Entity::template_same_heads`,
  `TemplateSpec::same_heads`, `std::vector<std::uint8_t>`).
  - Cause: GCC 16.2 writes a module unit exporting a struct with a `std::vector<bool>` member that
    its importers cannot read ("failed to read compiled module cluster: Bad file data").
  - Fix: belongs in GCC (unreduced, #1705). Use `std::vector<bool>` when ycxx compiles itself.
- **No `std::vector<bool>` in `sema::DeductionGuide::defaulted`** (`std::vector<std::uint8_t>`, #316).
  - Cause: a precaution against the same GCC 16.2 module bug (#1705); `ycxx.sema` exports the
    struct. `parse::Program::Index` keeps `std::vector<bool>` members without it, so the trigger
    is narrower.
  - Fix: belongs in GCC. Use `std::vector<bool>` once #1705 shows it is safe, or when ycxx compiles itself.
- **The interpreter's byte comparison is defined in its implementation unit** (`src/interp/ops.cpp`).
  - Cause: with stock libstdc++, GCC 16.2 cannot read the `ycxx.interp:machine` partition when
    it contains the body of `Machine::compare_bytes`.
  - Fix: belongs in GCC. The definition can return to the partition when ycxx compiles itself.
- **Defaulted comparisons are members, not hidden friends** (`src/ast`, `src/ir`, `ycxx.hash`'s `Fp128`).
  - Cause: GCC 16.2 crashes (segfault) in an importer that compares two values of an exported
    struct whose defaulted `operator==` is a friend, with or without `-freflection` (found by
    #14, #23 and #32).
  - Fix: belongs in GCC. Restore the friend form when ycxx compiles itself.
- **Checked `constexpr` constructors of handles are not `constexpr`** (`make_node`,
  `make_qual_type`, `make_decl_ord` in `src/ast/handles.cppm`).
  - Cause: GCC 16.2 does not emit its TU-local contract-violation helper
    (`__tu_has_violation`) in an importer that inlines a non-template inline function with a
    precondition. The importer fails to link unless it has a contract of its own. Members of
    non-template classes are not affected; a class template's member is, when its specialization
    is instantiated in the imported interface (GCC PR 124264; #46).
  - Fix: belongs in GCC.
- **A contract predicate inside a lambda that names a member of the enclosing class goes through
  a local `bool`, or a member's precondition** (`src/ast` interner, builder, compaction;
  `pipeline::Session`'s registry providers).
  - Cause: in such a predicate GCC 16.2 takes `this` to be the closure object, so the member
    read is garbage. The lambda's own captures read correctly, and so does the same expression
    in the lambda body. It happens in a plain TU, with generic or plain lambdas, `contract_assert`
    or the lambda's `pre`, at -O0 and -O2:

        struct S { int x = 42;
          void f() { [this] { contract_assert(x == 42); }(); } };  // reported violated
        int main() { S{}.f(); }

  - Fix: belongs in GCC. Restore the plain form when ycxx compiles itself.
- **Checked functions of `ycxx.ast.typeops` and `wrap` are not `constexpr`,** for the reason
  above (an importer that inlines a checked inline function fails to link).
- **`Values::is_dependent` reads a table instead of calling `dispatch`.** GCC 16.2 ICEs
  (`gimplify_expr`) on `dispatch`'s `pre` when `is_dependent` calls it. Not reduced yet;
  `compact.cppm`'s calls of `dispatch` over the same list compile.
- **`pre` on a function template is dropped when it is instantiated through an import** (GCC
  16.2; non-template functions keep theirs). `Db::set` asserts its precondition with
  `contract_assert` in the body instead.
- **The std module's BMI must be built with `-freflection -fcontracts`.**
  - `CMakeLists.txt` adds the flags to libycxx's `ycxx_modules` target.
  - Without them, `std::meta` is empty. An importer whose `-freflection` does not match the
    BMI's segfaults GCC 16.2.
- **The embedded libycxx compiles its implementation units as plain translation units.** The
  scheduler's `import std` would otherwise make its runtime archive depend on the std module that
  already links it. `CMakeLists.txt` scopes `YCXX_SOURCE_MODULES=OFF` to the embedded build and
  keeps the public `std` and `std.compat` modules enabled. [#664](https://github.com/yrashk/ycxx/issues/664)
- **`FpIndex`'s `Shard` has a user-provided constructor** (`src/ast/store.cppm`, #283).
  - Cause: at -O1 and above GCC 16.2 segfaults in an importer of `ycxx.ast.interner`, whose
    `Interners` has an inline constructor and several `FpIndex` specializations, while
    constant-evaluating `shards_`'s initializer for a constexpr-constructible `Shard`. Debug
    builds are not affected. Two-file repro on #283; related to GCC PR 124973.
  - Fix: belongs in GCC. Make `Shard` an aggregate again when ycxx compiles itself.
- **`compact.cppm` is compiled with `-fkeep-static-functions`** (`src/ast/CMakeLists.txt`, #46).
  - Cause: at -O1 and above GCC 16.2 inlines `KindTable<…>::kind`'s `pre` into `Compactor`
    without emitting the TU-local violation helper it calls, so programs linking the AST fail
    to link (the template case of GCC PR 124264, above). Repro on #46.
  - Fix: belongs in GCC. Drop the option when ycxx compiles itself.
- **Release (-O3) false-positive warnings, restructured instead of disabled** (#726; restoring
  them is #748). Each comment shows the plain form to restore.
  - `-Wmaybe-uninitialized` on a `std::optional`'s payload read after a check: a value and a flag
    in `link::elf::read_shared_object` (`dynamic`, and `symbol_count` from `parsed`),
    `obj::archive::read` (`index`), `parse.cppm` (`named`, `mem`, `exception_tokens`, `lit`)
    and `Sema::class_name` (`found`).
  - `-Warray-bounds` through libycxx's in-place `vector` insert: reserved appends in
    `Sema::member_call` (`parameters`), `Sema::call` (`all`) and
    `Sema::check_placeholder_constraint` (`args`).
  - `-Warray-bounds`/`-Wstringop-overflow` through `"literal" + std::string&&`: `std::format` in
    `daemon/connect.cppm` (four messages).
  - `-Wstringop-overflow` on the `sun_path` copy: a copy also bounded by the array in the
    daemon's `socket_address`.
  - `-Wstringop-overread` on `string::find`: `pp::floating` keeps the position of `e`.


## Dependent immediate calls in template defaults (#1327)

An integral non-type template parameter default can call an eligible `consteval`
function with earlier non-type parameters. The unqualified or namespace-qualified
call selects its overload in the declaration context. Substituting an omitted
argument evaluates the selected call and checks the converted constant expression
for the destination parameter type. Explicit template arguments skip the default.

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

## Throwing a trivial class object on Linux (#1237)

A cleanup-free x86-64 Linux function can throw an explicitly initialized,
default-visible namespace-scope record with one ordinary public `int` member
and trivial implicit copy and destruction. The hosted Itanium ABI path allocates
the exception object and copies the target-layout bytes. The ELF object emits the
root-class type identity with both `-frtti` and `-fno-rtti`. Mixed GCC and Clang
links catch the record by const reference and observe the signed payload.

More general exception-object construction, native class handlers and exceptional
cleanup remain under [#89](https://github.com/yrashk/ycxx/issues/89) and
[#135](https://github.com/yrashk/ycxx/issues/135). Darwin parity is
[#648](https://github.com/yrashk/ycxx/issues/648).

## Ordinary block try and catch (#558)

The front end retains the protected compound and ordered handlers. A named
exception declaration shares the handler's outer block scope, and control-flow
checks reject entry into the protected block or a handler. A discarded
`if constexpr` arm skips the full handler sequence.

On Linux x86-64, a host-linked ELF object can contain an eligible namespace
function with a protected return that passes its parameter directly to an
externally defined `int(int)` function. One `catch (...)` can return an integer
literal, or one named by-value `int` handler can return its bound value.
L1 and L2 carry distinct normal and unwind results. The AMD64 emitter records
the call range and landing address, saves the unwind header and selector, and
feeds the ELF writer's handler CIE and LSDA. The typed LSDA uses the hosted
`_ZTIi` to match exceptions, and a different type continues unwinding. The
native final link diagnoses the hosted runtime requirement.
[Design](docs/design/linux-hosted-catch-all.md#hosted-by-value-int-handler-919)
· [#583](https://github.com/yrashk/ycxx/issues/583) · [#919](https://github.com/yrashk/ycxx/issues/919)

Same-translation-unit callees, other typed handlers, multiple or nested
handlers, rethrow, combined handler cleanup and freestanding runtime discovery
remain in [#89](https://github.com/yrashk/ycxx/issues/89).
Darwin parity is tracked in [#648](https://github.com/yrashk/ycxx/issues/648).

## Unevaluated calls to known-return function templates (#565)

An unevaluated call to a non-constexpr free function template can use its
declared return type after signature substitution and constraint checking.
Evaluated calls and calls that need placeholder return deduction still demand
the body. A non-generic lambda body uses its own evaluation context.

## Decay-copy expressions (#286)

- Single-expression `auto(x)` and `auto{x}` perform scalar, pointer and class decay-copy,
  including bit-field reads and array and function decay. A class operand is copied or moved
  by direct-initialization; a prvalue of the class initializes the result with no copy. Grouped declarations
  still take precedence where the statement can be a declaration.
- Function-template bodies replay decay-copy of supported dependent operands with the
  template arguments bound. A value-dependent operand with known type gives the conversion
  its decayed prvalue type in the template definition, including in `decltype`.
- Concrete decay-copy works in a non-type template argument with `auto{expr}` or with
  `auto(expr)` when the initializer starts with an integer literal. The argument must be
  a converted constant expression. Function and function-pointer type-ids keep precedence.
- A dependent operand gives a dependent prvalue in any templated context: requires-expressions
  and compound requirements (as libycxx's ranges CPOs write them), noexcept operators and
  specifiers, trailing return types and base specifiers. Substitution analyses the copy, so a
  deleted copy constructor makes a requirement unsatisfied.
- In a constraint, a decay-copy is written as a conversion to the `auto` placeholder, which
  mangles as `cv Da e` and `tl Da e E` as Clang and GCC do (`tests/own/abi/decay-copy-constraints.run.cpp`).
- Member pointers, other direct parenthesized template argument spellings such as
  `Tag<auto(n)>`, and dependent template arguments remain open.

## Comparisons: `<=>`, defaulted `==` and `<=>`, rewritten candidates (#295)

- A defaulted `operator==` ([class.compare.default], [class.eq]) may be a const member taking
  `const C&`, or a hidden friend taking two `const C&` or two `C`, of a non-template class. It
  returns `bool` and is implicitly constexpr. Other signatures are diagnosed.
- When its class completes, `Program::complete_defaulted_comparisons` runs first, before default
  arguments and member initializers are bound. It builds the comparison in a trial `Sema`
  (`defaulted_equality`), comparing the bases, then the data members, then each array element,
  joined with `&&`. The operator is deleted when a member is a reference or variant member, or
  when a comparison finds no viable, an ambiguous, a deleted or an inaccessible function. Any
  other diagnostic is ycxx's own limitation: the operator stays declared, and its body reports
  it. Without a written noexcept-specifier, the exception specification is the noexcept of that
  expression ([except.spec]). `completed_declaration` publishes both results, and so does the
  friend's `written_member`. `BodyAnalysis` builds the same expression as the body.
- `x != y` with no viable `operator!=`, and `x == y` with no viable `operator==`, try
  `operator==` for `x == y`, then reversed as `y == x`, and negate the result for `!=`
  (`Sema::rewritten_equality`, [over.match.oper]/3.4). This runs only when an operand has class
  type. The selected function must be a rewrite target (no corresponding `operator!=` in its
  scope), and its declared return type must be cv `bool`. A rewritten candidate is not yet
  ranked against a viable ordinary one, built-in candidates for enumerations included. Other
  rewrite targets are not tried when the selected one is not a target.
- The built-in `a <=> b` ([expr.spaceship], `Sema::builtin_three_way`) takes arithmetic operands
  after the usual arithmetic conversions (a narrowing one other than integral to floating is
  ill-formed), two of one enumeration as its underlying type, an unscoped enumeration with an
  integral type, and object pointers converted to their composite pointer type
  (`Sema::composite_pointers`, shared with the relational and equality operators). `bool` goes
  only with `bool`. The result is `std::strong_ordering`, or `std::partial_ordering` for
  floating operands, found by qualified lookup in `std` with its static members `less`,
  `equal`/`equivalent`, `greater` and `unordered` (`Sema::comparison_category`); without
  `<compare>` it is an error.
- `ast::ThreeWay` holds the converted operands and those members. IR generation
  (`Gen::three_way`) evaluates the operands once, compares them, selects the address of the
  member that names the result and copies it into the result object; a NaN leaves `unordered`.
  Discarded, only the operands are evaluated.
- `Sema::rewritten_three_way` ([over.match.oper]/3.4, /8): with no viable ordinary candidate
  and an operand of class type, or of scoped enumeration type where the built-in operator is
  not viable, `x @ y` for a relational `@` tries `operator<=>` as `(x <=> y) @ 0`, then reversed
  as `0 @ (y <=> x)`; `x <=> y` tries the reversed one, `0 <=> (y <=> x)`. The `0` is a
  one-token literal, so it is a null pointer constant too. A rewritten expression's own
  operator is not rewritten again.
- With viable forward and reversed candidates, `Sema::ranked_three_way` compares their
  conversion sequences in source operand order before preferring the forward orientation on
  a tie ([over.match.best.general], [#1223](https://github.com/yrashk/ycxx/issues/1223)).
  This covers fixed-arity, non-template members and nonmembers for integral operands and
  complete non-template class operands without bases or conversion functions. It compares exact
  bindings and by-value integral conversions. For relational expressions this ranking applies
  when lookup finds no ordinary relational operator declaration.
- A defaulted `operator<=>` ([class.spaceship]) is declared as `==` is. Completion
  (`Program::defaulted_three_way`) compares each subobject with `<=>`. A return type of plain
  `auto` becomes the common comparison category of the results (`strong_ordering` with no
  subobjects); a result that is not a category deletes the operator. Each result is converted to
  the return type R. A comparison category that cannot convert to an explicit scalar builtin R
  makes the operator deleted ([class.spaceship]/1.2, /2.2). The body is
  `{ R v0 = x0 <=> y0; if (v0 != 0) return v0; … return R(std::strong_ordering::equal); }`
  (`defaulted_three_way_body`). Completion and the body build the same initialized local and
  lvalue result test. Spec and Clang modes use `vi != 0`; GCC mode checks `vi == 0` and returns
  the result when the test is false. The implicit exception specification covers the comparisons, the
  lvalue tests, and initialization of either return value (#1225).
- `x.operator@(…)` and `p->operator@(…)` name member operator functions.
- For non-assignment binary operators without comparison rewrites, an access, deletion or
  ambiguity diagnostic from the member-only probe can trigger joint ranking with nonmembers.
  The recovery covers direct, non-template classes, exact bindings and integral conversions
  by value. It retains the member diagnostic when a member wins, reports ambiguity when no
  candidate wins, and preserves errors from using the selected nonmember. Comparison ranking
  continues in
  [#1223](https://github.com/yrashk/ycxx/issues/1223).
- Remaining: the `==` a defaulted `<=>` declares implicitly
  ([#1196](https://github.com/yrashk/ycxx/issues/1196)); the synthesized three-way comparison from
  `==` and `<` ([#1197](https://github.com/yrashk/ycxx/issues/1197)); conversions between
  comparison categories, which need conversion functions between classes, so a defaulted `<=>`
  over mixed categories fails ([#339](https://github.com/yrashk/ycxx/issues/339)); defaulted
  secondary operators, defaulted comparisons in class templates, out-of-class defaulting and
  joint ranking of rewritten candidates ([#295](https://github.com/yrashk/ycxx/issues/295)).
  Constant evaluation of the classes compared fails when they have a class or array subobject:
  [#1140](https://github.com/yrashk/ycxx/issues/1140).
- Tests: `tests/own/three-way-comparison/`. Most use `inputs/compare.hpp`, a stand-in for
  `<compare>`, because libycxx's `<compare>` stops at `<type_traits>` gaps (#776).
