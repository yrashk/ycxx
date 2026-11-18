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
2. **Bootstrap compiler:** Clang 23 (the version libycxx pins). GCC 16 must keep working as a
   second bootstrap compiler, so that no single host compiler's bugs shape the code.
3. **Self-hosting** is a milestone, not a starting assumption: ycxx compiles libycxx and passes
   its suite first, then compiles itself, and the stage-2 and stage-3 binaries must be identical.
4. **libycxx learns about ycxx through its single detection point** (`config.hpp`). Any builtin
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
