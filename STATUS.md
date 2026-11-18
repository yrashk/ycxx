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
  - `src/ir/fingerprint.cppm` adapts `ycxx.hash`. The handles `EntityId`, `QualType`, `ConstId`,
    `LocalId` and `Loc` are stand-ins in `src/ir/handles.cppm` until #32/#33 land; types print as
    `T<n>`.

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

## Mach-O objects and the linker (#42, 2026-11-18)

- `ycxx.obj` (`src/obj/`): the in-memory object. Sections are named the Mach-O way and hold bytes
  (or a zero-fill size) and relocations. Symbols are local, global or hidden, possibly weak.
  Relocations are `branch26`, `page21`, `pageoff12`, `got_page21`, `got_pageoff12`, `abs64`,
  `abs32` (differences only) and `got_rel32`, with explicit addends. Lists keep build order (§A5.3).
- `ycxx.obj.macho`: the arm64 MH_OBJECT writer and reader. The reader takes Clang and GCC objects
  as black-box inputs. A common symbol becomes a weak definition in its own `__DATA,__common`
  zero-fill section. The reader rejects absolute and indirect symbols, alignments above 2^15,
  scattered and TLV relocations, and section-relative relocations other than UNSIGNED.
- `ycxx.link` (`src/link/`): `link(objects, options) -> expected<bytes, errors>`.
  - Diagnoses duplicate and undefined symbols and a missing entry point. A strong definition beats
    a weak one; common symbols merge to the largest. `__mh_execute_header` and `___dso_handle` are
    defined by the linker. A weak reference that no dylib exports is imported weakly (0 at run time).
  - Imports come from the dylibs in `options` (libSystem by default), checked against export
    lists read from `.tbd` files (`tbd_exports`). A dylib without a list accepts any name.
  - Lays out `__PAGEZERO`, `__TEXT`, `__DATA_CONST`, `__DATA` and `__LINKEDIT` on 16 KiB pages,
    with stubs and a GOT. All imports are bound and all pointers rebased through chained fixups
    (`DYLD_CHAINED_PTR_64_OFFSET`). No lazy binding.
  - Writes the exports trie, symbol tables, `LC_MAIN`, `LC_BUILD_VERSION` (macOS 12.0), an
    `LC_UUID` fingerprinted from the image, and an ad-hoc, linker-signed SHA-256 code signature.
  - Output is byte-identical across runs.
  - Not yet: unwind info (`__compact_unwind` and `__eh_frame` input sections are dropped, so C++
    exceptions cannot cross ycxx-linked frames), thread-local variables, dead stripping, literal
    merging, branch islands beyond ±128 MiB, other dylibs' re-exports, incremental relinking, and
    import addends outside 0..255 (`DYLD_CHAINED_IMPORT_ADDEND`). `__LLVM` (embedded bitcode),
    `__LD` and debug sections are dropped.
- `build/bin/ycxx-ld a.o b.o -o out`: links objects from the command line, for tests. It reads
  libSystem's exports from the SDK (`--sdk`, `$SDKROOT` or the Command Line Tools' SDK).
- `unit.link`: SHA-256 vectors, object round trips, `.tbd` parsing, diagnostics, the exports trie
  and determinism on both hosts. On arm64 macOS it also runs hand-encoded programs (exit code,
  calls between objects, data, GOT and pointer relocations, `puts` from libSystem), checks
  `codesign -v`, and links a C object compiled by the bootstrap compiler.

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
  (segfault), with or without `-freflection`. `ycxx.hash`'s `Fp128` and `src/ir` use member operators.
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
