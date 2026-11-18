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
4. **Optimization is designed in from the start, and delivered after correctness.** The first
   back end aims for correct, debuggable code of roughly `-O0`/`-O1` quality. The architecture
   (§4.8) is laid out from day one for optimization by search, solvers and measured cost models,
   so that strong optimization arrives later without the front end or IR having to change. The
   goal is to compete with Clang and GCC on generated code, measured (§5.4).

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

## 4. Architecture (direction; details open)

These are the commitments. How each one is met is open until its design document lands.

1. **Resident, query-based core.** Every analysis result is a memoized query over inputs (file
   contents, command-line configuration). Queries record what they read, *including negative
   results*: failed lookups, overload sets considered, macros tested and found undefined. A
   change invalidates exactly the queries that observed it, and a recomputed query whose result
   is unchanged stops propagation (early cutoff). **Open: [query-engine design].**
2. **Incremental equals from scratch.** For any sequence of edits, the incremental result must be
   identical to a cold run on the final state. This is a test invariant enforced on every suite,
   not an aspiration.
3. **Futures and parallelism.** Queries are scheduled as futures on a work-stealing pool.
   Translation units, function bodies and template instantiations proceed in parallel.
4. **Determinism.** Output never depends on scheduling. Any ordering that the language or the
   output format exposes (points of instantiation, diagnostic order, symbol and section order,
   string table order) is derived from source positions and stable keys, never from completion
   order.
5. **Shared across translation units:** preprocessed and parsed headers (keyed by content and by
   the macro state they actually depend on), module interfaces and template instantiations.
6. **Interpreter first.** The constant evaluator executes ycxx's own IR. The same interpreter runs
   whole programs, so the front end can be validated by running tests long before a native back
   end exists.
7. **One binary, many fronts.** The `ycxx` binary is the daemon and also its clients: a
   `c++`-compatible command line, an LSP server and a control interface (reload, status, query).
8. **Optimization is search over verified transformations, and it runs while the compiler is idle.**
   - **Optimizations are queries too.** A function's optimized form is memoized, keyed by its IR
     hash, target, options and optimizer version. It is persisted to an on-disk cache, so work
     done once is never redone.
   - **Anytime optimization.** A build gets the best result available within its budget. While
     idle, the resident compiler keeps improving hot or recently changed functions in the
     background, and later builds pick that up. A fast development build and a strong release
     build differ in budget, not in pipeline.
   - **Search, not just fixed pass pipelines.** The mid-level optimizer is expected to use
     equality saturation (an e-graph of equivalent forms, with extraction by cost). Phase
     ordering, inlining and unrolling are searched or tuned against cost models, not hard-coded
     orders. Superoptimization of small hot regions is used where budgets allow.
   - **Solvers where the problem is combinatorial:** instruction selection as covering, register
     allocation and instruction scheduling as constraint or integer programs, e-graph extraction
     as ILP, code layout. Heuristics remain the default fast path. Solvers improve on them as the
     budget allows, never replace them.
   - **A constraint-model layer owned by ycxx** stands between the optimizer and any solver. It
     plays the role FlatZinc plays for MiniZinc. Solvers are back ends of that layer and
     interchangeable (§7), and the same model can be exported for off-line experiments with
     MiniZinc's own tool chain.
   - **Verification:** no search or solver result reaches an object file unless it is correct by
     construction (a rewrite rule proven once) or validated against its input, by SMT translation
     validation of the region. On failure or timeout the heuristic result is used.
   - **Determinism (§4.4) holds:** budgets are counted in deterministic units (solver conflicts
     or nodes, e-graph iterations), never wall-clock time, and parallel solvers run in their
     deterministic modes. A build is reproducible from its inputs plus its optimization cache.
     The cache can be exported and committed, so a release build can replay it exactly.
   - **Cost models are data, measured per target** (latency and throughput microbenchmarks run on
     real hardware), not constants guessed in code.

   **Open: [optimization architecture design]** (IR shape for e-graphs, the constraint layer's
   interface, the verification boundary).

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
   - Fable: architecture decisions, the hardest specification clauses ([basic.lookup],
     [over.match], [temp.deduct], [temp.point], [dcl.init], [expr.const], [class.mem] layout,
     Itanium mangling edge cases) and the triage of failures nobody understands.
   - Opus at medium effort: most implementation work, raised to high effort for intricate parts.
   - Smaller models: mechanical work (data tables, extraction scripts, bulk test triage).

   Issues that need Fable are labelled `model:fable`.

   **How Fable is reached:** sessions run on Opus, because a session cannot be started on a
   model above its parent's. Fable is used through subagents: the Agent tool with
   `model: "fable"`.
   - On a `model:fable` issue, the Opus session owns the claim, the issue bookkeeping, the
     writing and the code. It gives every decision-bearing question to a Fable subagent.
   - Each brief is self-contained: the question, the draft clauses by stable name, the relevant
     DECISIONS sections, and the rule never to read compiler source (§2.2). A subagent sees only
     its brief.
   - The session reviews what Fable concludes before using it, and pushes back with a follow-up
     to the same subagent when something is unclear or seems wrong. It does not silently
     override.
   - Design documents and decision comments say which conclusions came from Fable.
   - On issues without the label, the session calls Fable on its own judgment when it meets a
     question listed above.
6. **Design documents** live in `docs/design/` and are written before the code they govern. Once
   accepted, their conclusions are summarized in this file.

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
     `CLAUDE.md`.
   - A *recipe* for a recurring situation goes into [`docs/LESSONS.md`](docs/LESSONS.md), the
     list of common solutions.
3. **Every handover and closing comment answers one question:** *what would have made this
   faster?* If the answer is not trivial, it becomes an entry in `docs/LESSONS.md`, in the same
   PR or as a direct commit to `main`.
4. **Read before starting.** Sessions read `docs/LESSONS.md` along with this file. A situation it
   covers is handled its way, or the entry is corrected if it turned out to be wrong.
5. **Consolidate.** A lesson that recurs is promoted to a rule here or in `CLAUDE.md`, and its
   entry points at the rule. Entries that stop applying are deleted. Keep the list short enough to
   read in full.

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
