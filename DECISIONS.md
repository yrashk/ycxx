# ycxx design decisions

Each section records a decision and why it was made. A decision that is still open says so and
names the issue where it is being worked out. Change a section by committing the change with its
reasoning; do not leave decisions only in issue threads.

## 1. Scope

1. **Language:** the C++26 working draft, pinned to a commit of the draft's published LaTeX
   sources, plus the CWG issue resolutions that implementations are expected to follow. Annex C
   compatibility and Annex D deprecated features are implemented. C is compiled only to the
   extent that system headers need it inside C++.
2. **Extensions:** only those system headers and libycxx need, each listed in
   `docs/EXTENSIONS.md` with the headers that require it (GNU attributes, `asm` labels,
   availability and nullability annotations, the builtins libycxx probes for). An extension is
   added because a real header needs it, never for its own sake.
3. **Targets, in order:** AArch64 Mach-O (macOS), x86-64 ELF (Linux), AArch64 ELF (Linux). Each
   target has to be fully supported, including its object format, linker, unwind tables and DWARF,
   before the next one starts.
   - The first Linux x86-64 [dynamic link](docs/design/linux-dynamic-executable.md) uses
     explicit shared objects, the target interpreter and a fixed-address executable. Native
     direct function imports use eager GOT relocations. Provider discovery uses GNU hash when
     available and otherwise uses SysV hash. Target CRT, versioned imports, provider dependencies
     and normal hosted process startup follow under #390.
   - Linux x86-64 [archive inputs](docs/design/linux-static-archives.md) start with explicit
     indexed GNU archives in static executable links. The linker extracts only members needed
     by outstanding symbols and preserves archive operand order (#388).
   - arm64 macOS [archive inputs](docs/design/darwin-static-archives.md) take Darwin archives
     (BSD names, ranlib index) with the system linker's selection, observed as a black box: an
     archive serves references from any operand, weak references and tentative definitions
     select members, and the first archive in operand order wins (#842). Selection reads the
     ranlib index where the system linker reads member symbol tables (#925).
   - Linux x86-64 [archive production](docs/design/linux-static-archives.md#native-archive-production)
     uses the standalone `ycxx archive -o` front to create or replace a deterministic regular
     GNU archive in process. It validates inputs with the ELF object reader, retains the original
     payloads in operand order and indexes named nonlocal definitions, including weak and hidden
     definitions. Astra reviewed the design (#389).
   - Linux x86-64 [external addresses](docs/design/linux-external-data-addresses.md) use a GOT
     relocation in relocatable objects for data and ordinary functions. The native static linker
     relaxes the validated RIP-relative load to a direct address after symbol resolution
     (#414, #407).
   - ELF and Mach-O [bind ordinary C++ function symbols locally](docs/design/tu-local-parameter-symbols.md)
     when their mangled parameter types identify an unnamed-namespace type. The pipeline retains
     the source linkage and Itanium spelling. Astra reviewed the scope and ownership rule (#582).
   - Linux x86-64 [COMDAT inputs](docs/design/linux-elf-comdat-input.md) keep the first whole
     ELF group per signature in explicit-input static links. Selection precedes startup, layout
     and relocation. Astra reviewed the ELF validation, symbol handling and link mode boundary
     (#422).
   - Linux x86-64 [ELF symbol names](docs/design/elf-symbol-names.md) share immutable input
     string-table storage. Astra reviewed valid suffix references, name lifetimes and the
     reader's storage bound (#484).
   - Linux x86-64 [native `int` throws](docs/design/linux-int-throw.md) use the Itanium runtime
     in functions certified to have cleanup-free unwind frames. A hosted peer provides the
     runtime and catches the exception. Astra reviewed the lowering boundary (#578).
   - The arm64 Mach-O linker retains [cleanup-free DWARF frames](docs/design/macho-cleanup-free-unwind.md)
     in `__TEXT,__eh_frame`. The frame PC uses the usual address-difference relocation so it
     follows the backed code through layout and does not need a runtime text relocation (#72).
4. **Optimization is designed in from the start, and delivered after correctness.** The first
   back end aims for correct, debuggable code of roughly `-O0`/`-O1` quality. The architecture
   (§4.8) is laid out from day one for optimization by search, solvers and measured cost models,
   so that strong optimization arrives later without the front end or IR having to change. The
   goal is to compete with Clang and GCC on generated code, measured (§5.4).
5. **Predefined macros** (decided on #112, identity revised on #229). Beyond the draft's
   ([cpp.predefined]):
   - **Identity:** `__ycxx__`, `__ycxx_major__`, `__ycxx_minor__` and `__ycxx_patchlevel__`, and
     `__GNUC__` 4, `__GNUC_MINOR__` 2 and `__GNUC_PATCHLEVEL__` 1, the version Clang claims
     (#229). Not `__clang__`, `__llvm__` or `__GNUG__`.
     - *Why `__GNUC__`:* #112 left it undefined, to be revisited if the SDK's non-GNU paths
       proved unusable. They warned on every `<stdio.h>` (`sys/cdefs.h`'s "Unsupported compiler
       detected"), and Clang has shown for years that `__GNUC__` 4.2.1 commits a compiler to a
       known, small set of GNU extensions.
     - *Conditions:* every GNU extension the SDK's and libycxx's `__GNUC__` paths use is
       supported or diagnosed, and listed in `docs/EXTENSIONS.md` (§1.2). libycxx recognizes
       ycxx by `__ycxx__` before `__clang__` and `__GNUC__`, and every compiler-specific choice
       in it tests its `_YCXX_COMPILER_*` identity, so no GCC workaround applies to ycxx
       (yrashk/libycxx#28, §3.6).
     - *Not `__GNUG__`:* the SDK's `__DARWIN_NULL` would become `__null`, an extension nothing
       else needs yet.
   - **System headers:** a warning located in a system header is not reported, as Clang and GCC
     do (#229). A system header is one found in a system directory (`-isystem`, libycxx's, the
     SDK's; a `-I` directory that is also one of these counts as one), or one a system header
     includes, wherever it is found. A warning on a token a system header's macro produced is
     reported where the macro is used, and hidden where that is in a system header. Errors are
     always reported, and so is `#warning`, which Clang and GCC report from a system header too.
   - **Probes:** `__has_feature`, `__has_extension`, `__has_attribute`, `__has_cpp_attribute`
     and `__has_builtin` answer truthfully from ycxx's own tables, 0 until implemented. Headers
     take the paths those answers select, and an extension they turn out to need goes to
     `docs/EXTENSIONS.md` (§1.2).
   - **Target macros** are generated from `ast::Target`, the source of layout, and checked
     against Clang's for the same target (`tests/own/pp/predefined-target.pp.cpp`).
   - **Feature-test macros:** a `__cpp_*` macro is defined only for a feature ycxx implements,
     from `src/pp/predefined.cppm`'s table. A slice that completes a feature updates the table.
     `__cplusplus` is `202400L`, as Clang 23 and GCC 16 have it for C++26.
   - *Reason:* a header that tests for a compiler or a feature must get the truth about ycxx.
   - *Rejected:* mimicking Clang (its extensions would all be owed at once), and defining every
     feature-test macro now (libycxx would enable code sema cannot compile).
6. **Which classes pass in registers** (decided on #230). This is the rule for when a class is
   trivially copyable for the purposes of calls. Compilers disagree on it.
   - **Default, every target:** upstream Clang and GCC, the Itanium "non-trivial for the purposes
     of calls" wording ([class.temporary]/3, Itanium C++ ABI §3.1.2.3). Where they differ, upstream
     Clang (the oracle) decides. They differ on a class whose member can be neither copied nor
     moved.
   - **The switch, `-fclass-call-abi=apple`:** on arm64 Darwin, Apple Clang's behaviour, for code
     that links with Xcode-built objects. A class passes in registers when its destructor is
     trivial and its copy constructor is trivial or deleted, whatever its move constructors.
     Apple Clang 21 passes move-only and deleted-copy classes in registers; Clang 23 and GCC 16
     pass them by address.
   - **Data:** `ast::Target` carries each target's default (`call_triviality`) and whether it
     offers the switch (`apple_calls`). The option is part of the compile configuration
     (`pp::Config::call_triviality`), so it enters the unit's fingerprint and the daemon's
     configuration.
   - **Tests:** `tests/own/abi/call-triviality` checks the default against Clang 23 objects. It is
     an expected failure under GCC only because of those members. `call-triviality-apple` checks
     the switch against Apple Clang objects (`PEER-WITH: apple-clang`).
   - *Reason:* the oracles and the ABI document agree on the default, and the switch serves code
     that must link with the platform's own objects.

7. **Dependent variable-template-ids in symbols** (decided on #777). Clang and GCC mangle them
   differently in a function template's signature when the name is qualified or an argument is
   defaulted or a pack: Clang as written (`sr …E`, `gs`, no defaults, no `J…E`), GCC the
   `<source-name>` and the converted arguments. ycxx writes GCC's form.
   - *Reason:* values hold no spelling (ast.md §5.5), and Clang's form would need it. GCC builds
     the `libycxx.a` ycxx programs link against. Implicit instantiations are emitted in every
     object that uses them, so a mismatch with a Clang object costs a duplicate, not a link error.
   - *Rejected:* Clang's form (needs the qualifier and arguments as written in every value);
     Clang's form for unqualified names only (two rules, still wrong for defaults and packs).
   - The same holds for pack-index-specifiers `T...[I]` (#297). GCC writes a constant index as
     the converted `std::size_t` (`Lm0E`), a substituted parameter in that parameter's type
     (`Li1E`), and a bound pack as `J <args> E`. Clang writes the literal as spelled (`Li0E`) and
     a bound pack as a placeholder. ycxx writes GCC's form.

## 2. Provenance

1. **Allowed sources:** the working draft and its LaTeX sources, the CWG and LWG issues lists,
   WG21 papers, cppreference.com, the Itanium C++ ABI, AAPCS64 and Apple's arm64 ABI notes, the
   System V psABI, the Arm ARM and Intel SDM, the ELF, Mach-O and DWARF specifications, POSIX,
   and libycxx (our own).
2. **Forbidden sources:** the source code, headers and build-generated sources (such as
   generated tables) of other compilers,
   assemblers and linkers: Clang/LLVM, GCC and binutils, MSVC, EDG, lld, mold, ld64 and
   ld-prime, Zig's compiler, Cranelift, and compiler research tools built on any of them
   (superoptimizers, translation validators, combinatorial code generators). Not their design
   documents either. Published papers are allowed, including papers about those tools
   (equality saturation, combinatorial register allocation and scheduling, superoptimization,
   translation validation).
3. **Compilers as black boxes:** Clang 23 and GCC 16 may be *run*: their accept/reject verdicts,
   `-E` output, `-fdump-record-layouts`-style layout dumps, symbol tables, object files and
   program outputs are oracles. Disassembling *our own* objects with any tool is allowed.
   **Competitive analysis** is allowed: building a benchmark corpus with Clang and GCC at any
   optimization level and comparing run time, code size and compile time with ycxx's (§5.4).
   **Reading competitors' emitted machine code is gated and logged** (decided in #11). Clang's
   and GCC's output, not their source, may be read only under these conditions:
   - Only to diagnose a *measured* performance gap on a specific function of the benchmark corpus
     (§5.4), never by browsing or "to see how it's done" in general.
   - Each reading is logged on the issue working on that gap: the function, compilers, versions
     and flags, and a short note of what was observed.
   - What was learned enters ycxx only restated in our own terms: a cost-model fact, or a
     transformation justified from first principles or a published paper and cited as such. No
     instruction sequence is copied into code, tests or comments, and assembly listings from
     other compilers are never committed.
4. **Borrowed test suites:** Clang's and GCC's language test suites may be run unmodified as
   external oracles, fetched into a cache outside the repository. Their *test* files may be read
   to triage a failure. They are used for accept/reject and run results only, never for
   diagnostic wording. Nothing from them is committed.
5. **The model caveat:** stated in the README. A similarity analysis against the Clang and GCC
   front ends, with libycxx's method (`deps/libycxx/docs/similarity/METHOD.md`), will be built
   before 0.1. Its rendered output, which quotes other implementations, is off-limits to agents
   implementing ycxx.

## 3. Implementation language and bootstrap

1. **C++26 with libycxx.** ycxx is written in C++26 and uses libycxx (submodule
   `deps/libycxx`) as its standard library, with `import std;`. libycxx's preprocessor policy
   (`deps/libycxx/DECISIONS.md` §1) applies to ycxx's own sources too.
2. **Modern C++, as far as the bootstrap compiler allows.** ycxx exercises C++20–26 as heavily
   as the bootstrap compiler supports. The test is: *how would this be written in fully
   implemented C++26?* Write it that way, unless the bootstrap compiler cannot.
   - **Concepts and constrained templates,** not SFINAE, tag dispatch or `enable_if`.
   - **Modules** for ycxx's own code; no project headers.
   - **Reflection (P2996)** in place of code generators, X-macros and hand-written boilerplate:
     AST and IR node definitions, visitors, hashing and equality of query keys, serialization
     of the persistent cache, diagnostic tables, option parsing.
   - **Contracts (P2900)** for preconditions, postconditions and invariants, instead of
     assertion functions.
   - **Coroutines and `std::execution`** (senders and receivers) for the query engine's futures
     and scheduling.
   - **`constexpr`/`consteval`** for tables and anything computable at compile time, including
     constexpr exceptions.
   - Ranges and views, `std::expected`, deducing `this`, pack indexing, structured-binding
     packs, `#embed`, and whatever else from C++20–26 makes the code clearer.

   Where the bootstrap compiler lacks a feature or miscompiles it, the workaround is the
   smallest one that works. It is marked `// BOOTSTRAP: <feature>: <reason>` and listed in
   STATUS ("Bootstrap workarounds"). Each is removed once ycxx compiles itself. Avoiding a
   feature merely because it is new is not allowed.
3. **Bootstrap compiler: GCC 16.2 with `-freflection -fcontracts`.** It supports more of C++26
   than Clang 23, which has neither reflection nor contracts (`deps/libycxx/STATUS.md`). GCC
   16.2 runs on both development hosts: macOS arm64 and Linux x86_64. Clang 23 is not a
   bootstrap compiler. It stays a black-box oracle (§2.3) and the default compiler of the
   *programs* in differential tests.
   - Either GCC or Clang is an acceptable bootstrap compiler. The one that allows the most of
     C++26 is chosen, and that is GCC 16 today. Supporting both would limit ycxx to their
     intersection.
   - If Clang overtakes GCC (reflection and contracts land, and its support is better
     overall), switch the bootstrap compiler with a DECISIONS change. Do not add dual support.
4. **Self-hosting** is a milestone, not a starting assumption: ycxx compiles libycxx and passes
   its suite first, then compiles itself, and the stage-2 and stage-3 binaries must be identical.
   From then on ycxx is its own bootstrap compiler, and its sources may use all of C++26,
   including what GCC lacks.
5. **The bootstrap chain.** ycxx can always be built from scratch, starting from GCC:
   - **The anchor release.** When ycxx first compiles itself (M8), the version that GCC 16 can
     still build is released and tagged as the bootstrap anchor. Its sources build with GCC, and
     the ycxx it produces builds the next release.
   - **Every later release is built by an earlier one.** Each release names the release that
     builds it (normally its predecessor). The chain from the anchor to the newest release is
     never broken. A source change that the previous release cannot compile waits for a
     release that can, or lands with a `// BOOTSTRAP:` workaround (§3.2) until then.
   - **CI walks the chain.** Each release is checked to build with its named predecessor, and
     the anchor is checked to still build with the pinned GCC. The full chain from GCC to `main`
     is rebuilt periodically, and the result must equal a normal build (stage-2 equals stage-3
     at every link).
   - **The anchor's GCC is pinned.** The exact GCC version and its flags are recorded with the
     anchor. If that GCC becomes unbuildable, a new anchor is released instead.
6. **libycxx learns about ycxx through its single detection point** (`config.hpp`). Any builtin
   libycxx needs from ycxx comes from libycxx's existing list of probes. If a new one is needed,
   it is added to libycxx upstream first.

## 4. Architecture

These are the commitments. §4.1–4.5 and the query parts of §4.8 are specified by the accepted
[query-engine design](docs/design/query-engine.md) (#1). References of the form §A1 or §B6 point into that document. The rest of §4.8 is
specified by the accepted [optimization design](docs/design/optimization.md) (#3), cited as §O1–§O14.

1. **Resident, query-based core.**
   - **Red/green queries with early cutoff by fingerprint.** Every analysis result is a pure,
     memoized query over inputs (file contents, configuration). It reads the world only through
     recorded fetches. Dependencies are ordered groups; validation verifies every member of a
     group and re-executes at the first changed group. Cutoff compares 128-bit Merkle
     fingerprints, so it survives eviction and restarts. Durability levels keep untouched system
     headers green in O(1). *Reason:* pay only for what an edit can change, deterministically.
     (§A1)
   - **Every semantic observation is a question keyed by exactly what was asked**, with negative
     answers as values: names in one scope at one point, member lookup, ADL, completeness, one
     macro, one include resolution. Overload resolution is keyed by candidate set and argument
     shapes. *Reason:* adding a declaration invalidates only the answers it could change. (§B2–§B9)
   - [Argument-dependent lookup](docs/design/ordinary-argument-dependent-lookup.md) searches
     the namespaces associated with argument types at the lexical point. Dependent calls also
     use the existing instantiation context. The search supplies candidates to overload
     resolution and provides the ADL-only fallback for range-based `for` (#165).
   - [Braced arguments](docs/design/braced-overload-resolution.md) contribute conversion sequences
     before overload selection. Parameter initialization and narrowing checks apply to the
     selected function (#231).
   - **Identity without positions.** Items are cut from the preprocessed tokens before parsing.
     Their ids exclude bodies and positions, and locations are `(item, offset)`. Declaration ids
     and structural entity ids are separate. *Reason:* edits elsewhere disturb no keys. (§B1, §B2)
   - **Cycles are internal errors.** Every C++ "in progress" state is a position or a key
     argument. Cycles are detected on a wait-for graph and reported deterministically. *Reason:*
     any recovery value would depend on entry order. (§A2)
2. **Incremental equals from scratch.** For any sequence of edits, the incremental result is
   identical to a cold run on the final state. This is a test invariant enforced on every suite.
   - Edit generators: random, mined from git history, delta-minimized.
   - Outputs are compared, and so is the memo table, through `ensure_green` with zero
     executions.
   - Runs repeat at several thread counts under a seeded chaos scheduler.
   - `--verify-memo` re-runs green cells to find missed dependencies.
   - Cancellation is injected at random points.
   *Reason:* a missed dependency is otherwise invisible until it miscompiles. (§A9, §B12)
3. **Coroutines and senders.**
   - Query executions are coroutines, and a cell's future is a `std::execution` sender, from
     libycxx.
   - The pool is a scheduler that also backs `parallel_scheduler`, with interactive and
     background priorities. Waiting suspends a frame and frees its worker.
   - `cheap` kinds, a closed list of prefix-only questions, are synchronous, so the hit path
     stays at about 100 ns with no allocation.
   - Cancellation uses stop tokens, and revisions advance only at quiescence: one `counting_scope`
     per epoch, joined before inputs change. Nothing half-computed is stored.
   *Reason:* parallel without blocked threads or partial results (§3.2). (§A3, §A4)
4. **Determinism.**
   - Every order that reaches an output comes from stable keys, and parallel results are
     positional.
   - Diagnostics are values: only errors that hold in every context. `tu(TU)` renders and orders
     them, with "required from here" chains taken from demand lists.
   - Pointer order, unordered iteration, `std::hash`, thread ids and clocks are banned in queries.
   *Reason:* output is bit-identical at any thread count. (§A5, §A1.7)
5. **Shared across translation units through contextual queries.**
   - A header analysis, an instantiation or a constant evaluation is a selection cell keyed by
     `(index, ctx)`, where `ctx = (prefix, pins, chain)`. A prefix is a content-addressed
     description of everything before the point, rooted at the configuration.
   - When a selection cell must execute, it first walks a per-index trie of verifying traces, and
     reuses any result whose questions get the same answers.
   - Function templates are instantiated at the end of the TU, and class and variable templates
     at the demanding site. A class template specialization that would differ later is pinned to
     its first instantiation ([temp.point]/7).
   - Divergence that is ill-formed, no diagnostic required, is diagnosed. Divergence across TUs
     is reported by the derived `odr_report`.
   *Reason:* results are shared wherever every question asked agrees, not only for identical
   preambles. (§A1.9, §B1.4, §B6, §B10)
6. **Interpreter first.** The constant evaluator executes ycxx's own IR. The same interpreter runs
   whole programs, so the front end can be validated by running tests long before a native back
   end exists.
   - [Stored typed array pointers](docs/design/ast.md#55-constants) retain their array subobject
     and position for constant evaluation, including one-past positions. Native relocations
     use the root symbol and byte addend (Astra, #111).
7. **One binary, many fronts.** The `ycxx` binary is the daemon and also its clients: a
   `c++`-compatible command line, an LSP server and a control interface (reload, status, query).
8. **Optimization is search over verified transformations, and it runs while the compiler is idle.**
   Specified by the accepted [optimization design](docs/design/optimization.md) (#3); references
   of the form §O2 point into it.
   - **Optimizations are queries too.** `optimized(unit_fp, target, options, optimizer_version,
     level)` is a pure memoized query, persisted to the on-disk cache. The unit is the function's
     lowered IR with its inline candidates. `target` includes the ISA feature set and the cost
     model's fingerprint. `optimizer_version` covers the rules, their proofs, the budget ladder,
     the pattern tables and the hash of every build input. *Reason:* work done once is never
     redone, on any machine. (§A8, §O2.4, §O7)
   - **One IR at three levels.**
     - L1 is semantic. The front end writes it, and the interpreter runs it in `constant` or
       `runtime` mode. It makes every [expr.const] fact explicit.
     - L2 is lowered: flat bytes with poison. The optimizer and translation validation work on it.
     - L3 is the machine level.

     Effects are threaded state values, and pure nodes float, so a function's dataflow is an
     acyclic DAG. *Reason:* the evaluator needs C++ object semantics, and SMT needs bit vectors.
     (§O2)
   - **Anytime optimization.** While idle, the daemon improves hot or recently changed functions
     at higher levels into an optimization cache. A build's output is a function of three things:
     its inputs, the requested level, and an optimization-cache snapshot that is an engine input.
     `-O0` pins level 0 exactly. The performance oracle's gaps order the daemon's work list, which
     never reaches a value. *Reason:* builds improve over time and stay reproducible. (§A8.2–A8.3,
     §O7, §O9)
   - **Equality saturation at L2, on equivalences only.**
     - Rules are data. They are proven once per optimizer version by SMT, under a poison-aware
       semantics with no `undef`.
     - Facts that hold only on some paths, and rewrites that only refine, go to a directed
       pre-pass that is translation-validated.
     - Extraction is greedy everywhere, plus exact ILP on e-graphs of up to a few hundred e-nodes.
     - Pass orders are searched over a small fixed set.

     *Reason:* congruence is sound only for equivalences. The spike measured greedy extraction
     at up to 26% worse than ILP, and ILP stops being practical near 1,000 e-nodes. (§O3, §O6.1)
   - **Solvers where the problem is combinatorial, behind `ycxx::cm`.**
     - `ycxx::cm` is ycxx's own flat constraint model, exportable to FlatZinc.
     - Back ends: HiGHS (MIP), CaDiCaL (SAT), Z3 (SMT) and Gecode (CP). All are MIT-licensed and
       built from source against libycxx, and all run single-threaded with deterministic limits.
     - Chuffed and CP-SAT are reached only through the FlatZinc export. Bitwuzla is used offline
       only, because it needs LGPL GMP.
     - Heuristics always exist and are the fallback. The heuristic and combinatorial instruction
       selectors and allocators read one pattern table and end in the same validators.

     *Reason:* solvers improve on heuristics but never replace them, and they stay replaceable.
     (§O4, §O5)
   - **Verification boundary.**
     - Proven rules are correct by construction.
     - Solver answers are checked: extractions for feasibility, allocations by a Rideau–Leroy
       validator, schedules by a dependence check.
     - Other passes are SMT-validated per acyclic region where the level's budget allows.
       Anything else is trusted and listed.
     - Evidence is part of the value, and replay re-checks it. A failed re-check under `--replay`
       is a hard error.

     *Reason:* the same guarantee as validating everything, at a fraction of the cost. (§O6)
   - **Determinism (§4.4) holds.**
     - Budgets are levels on a ladder, counted in deterministic units (e-graph iterations and
       nodes, conflicts, `rlimit`, branch-and-bound nodes), never wall-clock time.
     - E-graphs are built in id order and are never shared across tasks.
     - Solver solutions travel in values, so a replay re-checks them and never re-solves.
     - A stopped solver stores nothing. The part of the cache a build used is exported as a
       replay manifest, so a release build replays it exactly.

     *Reason:* the spike showed that matching in hash-table order changes results, and that a
     solver's search path depends on its binary. (§A8.4, §O3.6, §O4.2)
   - **Cost models are data, measured per target.** There is one table per microarchitecture,
     measured with latency and throughput microbenchmarks. Each table is a high-durability input
     whose fingerprint is part of `target`. (§O8)
   - **The first back end needs none of the search machinery.** M4 builds L1, the interpreter and
     the reflected op table. M5 builds `lower`, the pattern table, maximal-munch selection, linear
     scan, the validators and the §A8 plumbing. Neither needs an e-graph, rule, solver or SMT
     query. *Reason:* M4/M5 are not blocked on optimization work. (§O10)

9. **AST, entities and types.** Specified by the accepted [AST design](docs/design/ast.md) (#2); the
   section references in this item point into it.
   - **Handles, not pointers.** Every reference is a typed 32-bit handle into a process-wide
     interner, or an 8-byte structural id (`DeclId`, `Loc`). Values hold nothing context-specific;
     fingerprints, never handles, cross process boundaries. *Reason:* sharing across TUs and
     restarts (§A1.9, §A7). (§2)
   - **The parser analyses as it parses,** inside `item_sema` and `analyze_body`; the splitter
     yields lookup-free skeletons. *Reason:* C++ cannot be parsed without knowing which names are
     types and templates. (§3)
   - **One hash-consed tree family** with implicit operations and dependent constructs as kinds.
     Nodes hold no position; a child edge holds its offset relative to the parent, and a red view on
     the stack computes positions. *Reason:* equal subtrees are one node; the IR needs implicit
     operations explicit. (§3)
   - **Implicit copy assignment** of ordinary class members uses member overload selection and
     a synthesized body when required. The AST preserves operand order and carries the proof
     for trivial class copies. Astra reviewed the [assignment design](docs/design/implicit-copy-assignment.md) (#360).
     The same signature classifier supplies the effects of user-declared assignment on
     implicit constructors and call triviality (#244).
   - **Identities.** `DeclId = (ItemId, ordinal)` for written, synthesized and injected
     declarations, none inside bodies; structural `EntityId`s that include module attachment and
     trailing requires-clauses, with local entities numbered as the Itanium ABI does. *Reason:*
     ids survive body edits and edits elsewhere. (§4)
   - **Types:** one closed list of canonical and sugar kinds, cv in the handle's low bits, and the
     canonical form as a derived array filled at interning. Expressions carry the sugared type.
     *Reason:* O(1) canonicalization that does not depend on interning order, and diagnostics in
     the user's spelling. (§5)
   - **Templates.** Dependent values in canonical positions are `VDep*` constants, so no tree
     enters a type or a key. Declarations and canonical positions (signatures, deduction,
     partial ordering, entity ids) are substituted structurally. Bodies and every other part with
     tokens of its own (member-specifications, initializers, default arguments) are instantiated
     by analysing the pattern's tokens again with the arguments bound: non-dependent names at the
     pattern's own context, dependent ones at the instantiation's. Either way the result is a
     fresh tree that keeps the pattern's offsets. *Reason:* keys stay small and eager;
     diagnostics point into the template; one analysis path serves definitions and
     instantiations, so a construct sema learns works in instantiations at once. (§5.5, §6;
     amended by #88, decided by Fable)
   - **C++26:** reflections are constants over entity, type, value and positional handles;
     expansion statements store the pattern or the instances; contracts are declaration data.
     (§7)
   - **Locations:** tokens carry none; the region keeps a table of byte offsets and macro
     expansions, which is part of the tree id. `Loc` offsets count tokens, and positions that
     become constant values go through the recorded question `token_location`. *Reason:*
     reformatting moves no `Loc` and no item id. (§8)
   - **Memory:** trees are built in the frame arena and interned when the frame commits, into
     per-kind pools, never a `std::tuple` over the kinds; keys are interned eagerly. *Reason:*
     speculative parses and deductions leave no garbage. (§9)
   - **Closure types** ([expr.prim.lambda]) are unnamed class entities local to the enclosing
     entity, numbered per signature in closing-brace order as the Itanium ABI names them; the
     lambda's body is analysed inside the enclosing part, captures resolved through a frame per
     lambda into fields of the closure, and its members emitted with that part (#280,
     [closures.md](docs/design/closures.md), decided by Fable).
     An internal namespace variable can own a captureless closure. Its declarator anchors the
     identity before type deduction, and the declaration part carries its members (#338).
10. **The resident daemon.** Specified by the accepted [daemon design](docs/design/daemon.md)
    (#83); the section references in this item point into it.
    - **One daemon per project root and ycxx binary.** The root is the first `.ycxx` or
      version-control marker found walking up from the source (submodules skipped, worktrees
      counted). *Reason:* one revision per daemon, so one project's edits never stop another's
      builds, and two binaries never share a daemon. (§2)
    - **A Unix socket named by scope and build hash; the client writes the outputs.** One
      `compile` request carries the whole command line, made absolute against its `cwd`, and the
      answer carries stdout, stderr, exit status and output bytes. A disconnect stops the request.
      *Reason:* drop-in `c++` behaviour lives in one code path. (§3)
    - **Batches apply at once; stopped requests restart inside the daemon, behind a progress
      quantum** that grows only while restarts make no progress. *Reason:* LSP latency, no
      livelock, and the policy chooses only the revision a request is answered at. (§4)
    - **Inputs are provided on first fetch from a file table that changes only between epochs.**
      A batch is a set of dirty paths; a path no cell knows is dropped, so build outputs never
      stop a build. A request starts after a cookie on the watcher's queue shows every earlier
      change applied. (§5)
    - **Editor buffers are a second view in the same engine,** at a new lowest durability, so a
      keystroke leaves every build cell green in O(1) and builds never see unsaved text. (§6)
    - **Fallback to in-process on any failure; a crashed daemon is retried once.** Idle exit, and
      the persistent cache handed to the next daemon as red/green from a different start. (§7)
    - **Every answer carries its revision and per-root counters,** which the β tests check, with
      daemon and in-process outputs compared byte for byte. (§8)

## 5. Development process

libycxx's process (`deps/libycxx/DECISIONS.md` §6) applies, adapted:

1. **Small commits**, one logical change each, with oracle count changes in the message where
   relevant.
2. **Review every commit (P0–P2)** with libycxx's severity levels. Review fixes go in a separate
   follow-up commit.
3. **An independent, spec-only test author** writes ycxx's own tests from the draft and
   cppreference.com only. That author never sees ycxx's implementation or other compilers'
   tests. A failing own test is a compiler bug until the draft shows otherwise.
4. **Oracles,** strongest first:
   - the draft's own examples, extracted with their `// OK`/`// error` annotations;
   - differential runs against Clang 23 and GCC 16 (accept/reject, `-E`, program output);
   - mixed-object ABI tests: ycxx objects linked with Clang objects, exchanging exceptions,
     virtual calls, by-value classes and RTTI;
   - libycxx's suites and real-world projects compiled with ycxx;
   - the borrowed Clang and GCC language suites;
   - incremental-versus-cold equality over generated edit sequences;
   - self-hosting fixed point;
   - **the performance oracle:** a pinned benchmark corpus built by ycxx, Clang 23 and GCC 16
     at matching levels. Run time, code size and compile time (cold and incremental) are tracked
     per commit. Regressions are bugs, and the gap to the best competitor per benchmark is the
     optimizer's work list.
5. **One test driver** (as libycxx's `tools/test`): traceable transcripts for passing tests too,
   with HTML and Markdown reports.
6. **A new test or check is seen failing before it is trusted.** Break the code it guards, or
   revert the fix, and watch it turn red. Choose a break that some test input must reach, at the
   layer the data actually flows through. A test that passes at once may test nothing. Checks
   that could not fail were found on #19, #33, #96/#97, #247 and #261 (LESSONS, "Evidence and
   tests").

## 6. Coordination

Work is shared between many agent sessions, some running at the same time. The state of the
project must survive any session ending.

1. **GitHub issues are the source of truth for work in progress.** Every unit of work larger than
   a single commit has an issue. Its body is kept current as a living status: what is done, what
   is next and what is blocked. `STATUS.md` records what is *true of the code*; issues record
   *who is doing what*.
2. **Claiming:** a session claims an issue by assigning `yrashk`, adding `status:claimed`, and
   posting a comment `Claimed by <session name> (<branch>)`. It then re-reads the issue. If
   another claim comment came first, the earlier one wins and the later session backs off.
   A claim with no comment or commit for 48 hours may be taken over, with a comment saying so.
3. **Handover:** a session that stops before finishing posts a handover comment covering its
   state, its branch, what it learned and the next concrete step. It then removes
   `status:claimed`.
4. **Areas:** `area:*` labels partition the code. Two sessions never write the same area at the
   same time, and each works in its own git worktree and branch.
5. **Model allocation:**
   - Routine implementation, code review and testing use Claude Opus or Codex Sol at medium
     effort. Use high effort for intricate work. Smaller models can handle mechanical work
     such as data tables, extraction scripts and bulk test triage.
   - Use the corresponding design model sparingly: Claude Opus escalates to Fable and Codex
     Sol escalates to Astra. Escalate architecture and design decisions, hard specification
     questions that affect a design or implementation, and independent verification of a
     proposed design. The design model does not write code, review code, write tests or do
     routine triage.
   - Escalate a coding problem only after two serious attempts at a bug or implementation
     have failed to converge or explain the failure. The brief states what was tried and
     why it failed. The design model diagnoses or decides. The implementation session writes
     and verifies the code.
   - An issue labelled `model:fable` requests design-model involvement. Use Fable in Claude
     and Astra in Codex. Give the design model only the decision-bearing questions, in a
     self-contained subagent brief with the relevant decisions, draft clauses by stable
     name, and the rule never to read compiler source (§2.2).
   - The implementation session owns the claim, issue bookkeeping, writing and code. It
     checks the design model's conclusions before using them, follows up on unclear or
     disputed points, and attributes the conclusions in design documents and issue comments.
6. **Design documents** live in `docs/design/` and are written before the code they govern. Once
   accepted, their conclusions are summarized in this file.
7. **No follow-up is lost.** A follow-up is any work named but not done: a deferred review
   finding, a "later" or "out of scope" note, a known limitation, a stand-in to replace, a bug
   found in passing. Wherever one is written (a PR body or comment, a review, a handover, an
   issue comment, a session report), it is routed at once:
   - to an existing issue, with a comment there that states it and links back to where it came
     from; or
   - to a new issue with an `area:*` and a `kind:*` label, its milestone, a failing example where
     there is one, the clauses by stable name, and a link back to its source.

   Either way the source then links to that issue. A follow-up that lives only in a comment is a
   defect. A handover, closing comment or PR description is incomplete until every follow-up in
   it carries its issue link, and the coordinator does not merge a PR until it does.
8. **The README tells the truth about status.** Its Status section is an overview, not a log: what
   a user can do today, grouped by area (what compiles, on which platforms, which tools), what is
   not done yet, and which slices are in progress, with links to their epics, under one "as of"
   date. Per-change detail belongs in STATUS.md and the issues. A PR that changes what a user can
   do, or completes, starts or drops a slice, updates that overview in the same PR, editing it in
   place rather than appending; the coordinator does not merge it otherwise. The README never
   describes a goal as if it were already working.

9. **The feature map is the source of the roadmap** (#282). It lists every feature of the pinned
   draft at the grain a user names, ranked older and more used first, with its status on `main`
   and its issue. Its data is `tools/feature-map/features.toml`; `tools/feature-map/feature-map`
   generates the tables. The pinned tracking issue #282 shows them, and CI rewrites its body on
   every push to `main`.
   - **Slices come from its work list,** in rank order, unless a dependency or a milestone (such
     as self-hosting needing modules, reflection, contracts and coroutines, §3.4) says otherwise.
     The roadmap (#12) records each choice.
   - **A PR that completes or advances a feature changes only its row in `features.toml`**
     (status, note, issues). It commits no generated file: `probes.tsv` and the rendered table
     live in `build/feature-map/`, CI's artifact (#366). The coordinator does not merge a PR
     whose CI shows a status its probe contradicts.
   - **CI checks it.** `feature-map check` in `tools/check-policy` fails when a leaf clause of the
     pinned draft's core chapters is cited by no row, or a row that is not done has no issue. On
     macOS, after the build, `feature-map probe` and `check --probes` fail the run when a probe
     passes for a row marked missing or fails for one marked done. Moving the draft pin adds
     rows for new clauses; a recount (`feature-map fetch`, `count`) commits `counts.tsv`.
   - *Reason:* issues used to come only out of slices, so features no slice touched had none:
     `auto` and lambdas until θ, and default arguments, nested classes, inheritance and virtual
     functions, `new`, range-based `for` and block-scope statics until the map. Generated files
     committed by each PR conflicted between every two parallel feature PRs (#366).

10. **The native back ends stay at parity** (#639). AArch64 and x86-64 grew slice by slice, and
    by 2026-11-21 about 100 run tests compiled for one and not the other, unnoticed, because the
    run suite runs only on macOS.
    - A construct one back end supports and the other does not is a gap with its own issue,
      listed in the parity table #639.
    - `tools/check-parity` (policy stage) fails when an L2 operation is handled by one back end
      only and `tools/parity/l2-gaps.tsv` does not name its issue, or when a listed operation is
      now handled.
    - A test limited to one target carries `PARITY:` with the other target's gap or the
      design reason, as `tools/check-os-gates` requires for OS gates (#659).
    - A PR that adds a feature to one back end only files the other's gap and adds its row.
    - *Reason:* a gap that is recorded where the change is made is cheap to keep; a gap found
      later costs a survey.

## 7. Third-party libraries

"The whole loop in one program" means ycxx never shells out to another toolchain's assembler,
linker or archiver. It does not forbid linking libraries that are not compilers. A third-party
library may be added when all of the following hold:

1. **It is not a compiler component** of the kind §2.2 forbids, and reading its source does not
   expose agents to compiler implementations.
2. **Its license is permissive and compatible** with Apache-2.0 WITH LLVM-exception (MIT, BSD,
   Apache-2.0, zlib). No GPL in the shipped binary.
3. **It builds from source in our tree** with our bootstrap compilers and libycxx, as a pinned
   submodule under `deps/`. That doubles as a real-world test of libycxx and, later, of ycxx.
4. **It sits behind an interface ycxx owns** (for solvers, the constraint-model layer, §4.8), so
   it can be replaced, and ycxx still produces correct code without it.

Candidates for the solver layer, to be evaluated in the optimization design:
- SAT: CaDiCaL, Kissat.
- MIP/LP: HiGHS.
- CP: Gecode, Chuffed (both MiniZinc solvers).
- SMT, for translation validation: Bitwuzla, Z3.
- Google OR-Tools' CP-SAT is the strongest CP solver, but it pulls in Abseil and Protocol Buffers.
  It is a later candidate, used through the FlatZinc-style export first.

## 8. Self-improvement

The way agents work on ycxx is itself something to improve. A session that wasted effort and found
its way out has learned something the next session should not have to rediscover.

1. **Notice inefficiency.** Signs include:
   - repeated retries of the same failing approach;
   - review rounds that keep finding the same class of problem;
   - waiting on something that was never going to arrive;
   - a tool or permission dead end;
   - work redone because a decision was not written down;
   - a subagent brief that came back off-target.
2. **Record the recovery.** When a session recovers from one of these, it writes down the
   symptom, what worked and the rule to follow next time.
   - A rule about *how to work* (process, coordination, models, review) goes into this file or
     `AGENTS.md`.
   - A *recipe* for a recurring situation goes into [`docs/LESSONS.md`](docs/LESSONS.md), the
     list of common solutions.
3. **Every handover and closing comment answers one question:** *what would have made this
   faster?* If the answer is not trivial, it becomes an entry in `docs/LESSONS.md`, in the same
   PR or as a direct commit to `main`.
4. **Read before starting.** Sessions read `docs/LESSONS.md` along with this file. A situation it
   covers is handled its way, or the entry is corrected if it turned out to be wrong.
5. **Consolidate, regularly and deliberately.** At least once a day of active work, the
   coordinator reads `docs/LESSONS.md` for lessons to promote to a rule here or in `AGENTS.md`.
   - **Promote a lesson when it has high impact.** It recurred, or one occurrence lost work or
     hours, and the rule it gives is short and checkable.
   - **Promote few at a time.** Each rule cites the entries it comes from, and those entries then
     point at it.
   - **When unsure, ask.** If it is unclear whether a lesson should bind every session, or its rule
     would add cost to every change, open an issue labelled `status:needs-decision` and ask the
     maintainers instead of promoting it.
   - **Prune the list.** Delete entries that stop applying and merge duplicates. Keep the list
     short enough to read in full.
6. **Break bureaucracy loops.** Process exists to catch bugs and to keep work recoverable. A step
   that does neither is waste, and loops of such steps are the biggest time sink agents have.
   Signs of a loop:
   - another review or verification round when the last round found nothing that changes
     behaviour;
   - evidence that nobody will act on: restated status, hand-written transcripts, PR bodies
     that repeat the diff;
   - documents growing faster than the decisions in them;
   - relaying and reconciling between agents on the same seam more than once;
   - waiting for an approval or confirmation that is not actually required;
   - re-arguing a recorded decision;
   - the same fact written in several places;
   - polishing the wording of something not yet accepted.

   To break out, ask: *what bug, decision or unblocked work does this step produce?* If the
   answer is none, skip the step, say so in one line and move on.
   - Settle questions with running code where possible. A failing test or a prototype ends an
     argument that a review round would only prolong.
   - Bound every ritual: one review per change unless it finds P0 or P1 issues (§5.2), and one
     place per fact (§6.1: issues for who and what, STATUS for what is true of the code).
   - Size a design document to its decisions: each decision with its reasons and the
     alternatives rejected, nothing more.
   - Reversible steps need no ceremony. Do them, and let the tests and CI catch mistakes.
   - Evidence produced by tools (test transcripts, reports) is cheap and stays. Evidence
     produced by hand has to earn its place.
   - Every loop that is caught goes into `docs/LESSONS.md`.

## 9. Continuous integration: fail fast

CI time is expensive when agents push often, and a slow red signal wastes every session waiting
on it. CI is built to report the most important failure as early as possible, and never to be
where a bug is first found.

1. **Local first.** A session runs `tools/test --fast` (the same first tiers CI runs) before
   pushing. CI confirms; it does not debug.
2. **A pyramid, most important first.** Within a run, stages are ordered by importance and
   cheapness, and a failed required stage cancels the stages after it:
   1. build and policy checks (preprocessor policy, provenance leak check, formatting);
   2. **tests that failed in the previous run** of this branch, and of `main` for a new branch;
   3. tests affected by the change, chosen from the dependency information, and eventually from
      ycxx's own query engine;
   4. the core oracles: the draft's examples and the own suite;
   5. the wider suites: differential runs and incremental-versus-cold checks;
   6. everything else in the tier.
3. **Tiers:**
   - **Pull request:** fast, targeted, first failure within about 5 minutes and the whole tier
     within about 15.
   - **Merge to `main`:** the full own and oracle suites on both hosts.
   - **Nightly:** sanitizers, the borrowed Clang and GCC suites in full, edit-sequence fuzzing,
     the benchmark corpus, and the bootstrap chain (§3.5).
4. **Parallel by default.**
   - Hosts and configurations run as a matrix.
   - Suites are sharded by recorded test durations, so shards finish together.
   - Toolchains and build outputs are cached, and later ycxx's own persistent query cache too.
5. **Don't run what cannot have changed.**
   - Path filters skip builds for changes to documentation only.
   - A newer push to the same branch cancels the older run.
6. **The test driver keeps the state CI needs:** per-test durations, the last failures per
   branch, and a stable test identity. That makes "failures first", sharding and selection of
   affected tests possible.
7. **Flaky tests are quarantined at once** with an issue, never silently retried until green.
8. **Speed is a tracked property.** CI wall time per tier is recorded like benchmark results, and
   a tier that drifts past its budget gets an issue.
