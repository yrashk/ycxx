# Lessons: common solutions

Recipes for situations agents keep running into (DECISIONS §8). Each entry gives the symptom,
what to do, and where it was learned. Read this file before starting work. Add an entry when you
recover from a time sink, fix an entry that turned out wrong, and delete one that no longer
applies. A lesson that becomes a rule moves to DECISIONS or AGENTS.md and leaves a pointer here.

## Orchestration and sessions

- **Don't commit a generated file that every feature PR regenerates.** Under the first §6.9, each
  feature PR committed the feature map's `probes.tsv` and rendered table. With five slices
  running, every merge to `main` made the other open PRs conflict in both files, and #358, #352,
  #364 and #359 each needed an extra merge round and CI run within an hour.
  - Do: commit only the hand-edited source (here a row in `features.toml`), and let CI generate
    the rest and check it, publishing the result as an artifact or an issue body. A merge driver
    cannot regenerate data that needs a build, and `merge=ours` keeps it silently stale.
  - Rule: DECISIONS §6.9. Learned on #366.

- **A session cannot run on a model above its parent's.** A session asked to run on Fable
  silently started on Opus.
  - Do: run sessions on Opus and reach Fable through Agent subagents with `model: "fable"`.
  - Now a rule: DECISIONS §6.5.
  - Learned on #1, 2026-11-17.
- **Squash onto the branch's own base, never onto a freshly fetched `origin/main`.** On #234 a
  `git reset --soft origin/main` after a fetch moved HEAD to a newer main while the tree stayed on
  the old base, so the squashed commit also reverted 548 lines of other people's work. Only
  `git diff --stat` before pushing showed it.
  - Do: squash with `git reset --soft $(git merge-base HEAD origin/main)` taken before any
    fetch, or the base recorded at branch creation, then check `git diff --stat HEAD~1` lists
    only your files. Recover from the reflog (`git reflog`) by re-parenting the tree.
  - Learned on #234.
- **Force-push may be blocked in a session**, so rebasing a pushed branch onto `main` fails.
  - Do: merge `main` into the branch instead. History stays readable, and nobody has to approve
    a force-push.
  - Learned on #1.
- **Cross-session messages wait** until the receiving session's current turn ends.
  - Do: send the message and carry on. Don't block on it, and don't resend it.
  - Learned while bootstrapping.
- **GitHub issue comments are the reliable status channel.** A session's transcript may not be
  readable from another session, and its turn summaries are truncated.
  - Do: keep the issue body current, and put conclusions in comments, not only in the session.
  - This confirms DECISIONS §6.1.

- **macOS CI runners are shared by every repository of the account** (five concurrent jobs on
  the free plan). libycxx's hour-long macOS jobs, run twice per push (on `push` and on
  `pull_request`), kept ycxx's PR jobs queued for over an hour.
  - Do: run `push` CI only on `main`, cancel superseded runs with a `concurrency` group, and keep
    macOS jobs short. Count a libycxx PR as about an hour of every repository's macOS capacity.
  - Learned on the α slice (#39), 2026-11-18.

- **Don't stack more than one step on an unmerged PR while main moves under it.** δ (#86) stacked
  four PRs (#123 → #131 → #141 → #144) while #122 reshaped the pipeline under them. Every move of
  `main` cost a merge into each PR in turn. The bottom PR's stand-in layout was rewritten into
  #122's queries, and a test GCC disagreed with had to be fixed at the bottom and carried up.
  - Do: once a PR is up, keep at most one step stacked on it, and start further steps only as the
    base lands. When another session owns a seam you need (here the layout query, #63), build
    against its announced shape, not a parallel stand-in.
  - Do: run the own tests with every oracle (`tools/test --cxx gcc core own`, then `--cxx clang`)
    before opening each PR. The oracle job is what blocks a merge, and one error per reject file
    keeps the two compilers in agreement.
  - Learned on #86.
- **Build on unmerged dependencies in a local integration branch, then cherry-pick.** #24 needed
  #36 and #38 while both waited for CI. A local-only branch merging both let the work build and
  test at once. When they landed, the commits, which touched only #24's files, cherry-picked onto
  `main` with two one-line CMake conflicts.
  - Do: keep your commits free of the dependencies' files, never push the integration branch, and
    agree any shared file (here `ast/nodes.cppm`) with its owner first. Carry the owner's diff
    verbatim, so whichever branch merges second merges cleanly.
  - Learned on #24.
- **A worktree-isolated agent's shell refuses commands it cannot prove are not git.** Loops over
  `$f`, a compiler named by a variable, or several `python3 - <<EOF` steps chained with `&&` were
  refused whole, which cost a retry each on #43.
  - Do: put loops and multi-step edits in a script in the scratchpad and run that one file; use
    `tools/test` with a path filter (`tools/test --cxx ycxx core own/slice`) to compile and run
    a set of tests.
  - Learned on #43.
- **Write the back half against hand-built inputs while its front waits.** #43 needed the parser
  (#52) for any end-to-end run. The back half (function_ir to a running executable) was written
  and tested first, from bodies built the way sema builds them. When #52 landed, the front half
  took one commit, and every slice test passed on the first run.
  - Learned on #43.

- **Parallel sessions file the same gap within minutes.** The macOS ↔ Linux survey filed #652
  ("default target is the host") one minute after a Codex session filed and claimed #651. A
  search for "default target" missed #651, whose title said "host OS and architecture".
  - Do: right before filing, list the newest issues (`gh issue list --state all --limit 30`)
    and search two or three phrasings. If a duplicate slips through, close yours, remove your
    claim, and put what you learned on the surviving issue.
  - Learned on #659.

- **A stacked PR merges into its base branch, not main.** #198 was stacked on #191's branch. The
  coordinator merged #191, then merged #198 without checking its base, so #198 landed in the
  already-merged branch and never reached main. The β epic was closed and the README described
  concurrency that main did not have, until the daemon session noticed (#226).
  - Do: before merging, check `baseRefName` is `main`, and retarget a stacked PR once its base
    merges. After merging, confirm the merge commit is an ancestor of origin/main.
  - Rule: AGENTS.md, "PRs and branches".
  - Learned on #139, 2026-11-18.

- **`gh pr merge --auto` merges at once in this repository.** Auto-merge is disabled for
  yrashk/ycxx, so `--auto` does not wait for checks: #721 merged before its macOS job had run.
  - Do: wait for the checks yourself (`gh pr checks N`), then merge without `--auto`, with
    `--match-head-commit <sha>` so a later push cannot slip in.
  - Learned on #721, 2026-11-22.

- **Commits pushed to a PR's branch after it merges never reach main.** #233's Apple-ABI change
  was pushed to `claude/byvalue-116` after the coordinator had merged #233, so it never reached
  main (the same trap as #198, from the other side).
  - Do: before pushing to a PR's branch, check that the PR is still open
    (`gh pr view N --json state`). Once it has merged, start a fresh branch from main for anything
    new, even a follow-up to the same issue.
  - Make the check gate the push: `[ "$(gh pr view N --json state -q .state)" = OPEN ] && git push`.
    `gh pr view … -q .state && git push` only prints the state and pushes anyway; that pushed to
    #753's merged branch, and the commit had to move to #770. It happened again on #921 (#842),
    whose review nits had to move to the next PR.
  - The coordinator's sweep also checks every merged PR's branch for commits that aren't on main.
  - Rule: AGENTS.md, "PRs and branches".
  - Learned on #116 and #230, 2026-11-18.

- **A PR green on yesterday's main can break today's.** #250's CI passed and it merged with
  #243 without a textual conflict. Merged, #243's concepts test failed: #250 made every unknown
  `__builtin_` name "not supported yet", and #243 treats "not supported" as a hard error inside
  a constraint. Two more PRs that day had CI from before a sibling touching the same files
  merged.
  - Do: before merging, if `main` moved after the PR's last CI run, run `tools/test --fast` on
    the PR merged with `main` (a scratch worktree, `git merge origin/main`), and merge only if it
    passes and `main` has not moved again.
  - Rule: AGENTS.md, "PRs and branches".
  - Learned on #229, 2026-11-19.

- **A new deduction or template mechanism meets the old features in the review.** Both P0s of
  #334 (`auto`) were interactions: a return in a discarded `if constexpr` branch deduced the
  type, and `&` of an overload set reached deduction and crashed. The P1s were the same kind
  (a member of a class template named in its definition, a variable template's dependent
  initializer).
  - Do: before the review, run every new deduction through discarded statements, overload
    sets and their addresses, the current instantiation's members, and a template's definition
    versus its instantiation, and give each a test.
  - The same held for lambdas (#280, #281): the P0s and P1s were odr-use of constants, arrays of
    classes, a plain lambda inside a generic one, and a generic lambda calling a function
    template. Do: combine the new construct with each neighbouring one it can nest in or call
    (lambdas in templates and templates in lambdas) before the review.
  - Learned on #279; confirmed on #280 and #281.
- **`tools/test <paths>` with no stage name runs every stage, `build` included.** Meant to
  check a few tests in a fresh worktree, it started a full build there.
  - Do: name the stages, as in `YCXX=<binary> tools/test core own/pp/x`, or `policy` alone.
  - Hit again on #218, with oracles (`--cxx clang`): it built beside a background `tools/build`,
    and two Ninja runs in one directory broke a module ("imports must be built before being
    imported"). The rebuild cost 15 minutes on a loaded machine. Oracle runs need no build either.
  - Hit again on #309: a test-writing subagent's oracle run built for 37 minutes beside the
    implementer's build. Give a test author's brief `tools/test --cxx clang affected <path>`.
  - Learned on #659.
- **A reviewer running your binary blocks your rebuilds.** On #337 the reviewer probed with the
  worktree's `build/bin/ycxx`, so the next step could not be built for 15 minutes.
  - Do: build the next step into a build directory in the scratchpad
    (`YCXX_BUILD_DIR=<scratchpad>/build-b tools/build --no-tests`, about 30 s from a warm cache) and
    run tests with `YCXX=<that>/bin/ycxx tools/test --state <scratchpad>/state-b …`.
  - Learned on #337.
- **Editing sources during `tools/test --fast` spoils the run.** On #218 two fast runs were
  stopped, about 15 minutes lost, because a source edit landed while their build stage was
  compiling the same build directory: the result would not have described any commit.
  - Do: commit, start the fast run, and spend its 10 minutes on documents, issues and the PR body.
    To keep coding meanwhile, build into a scratchpad build directory (the entry above).
  - Stopping a `tools/test` that waits on a running `tools/build` in the same directory leaves
    half-built modules ("failed to read compiled module"). Let the build finish, then rebuild.
- **On a loaded machine the fast gate fails with timeouts, not failures.** On #330, with other
  sessions building and load averages of 120–250, two fast runs failed. The CTests had 10 s
  executable timeouts, `daemon.trace_tui` has a 2 s budget, and draft tests that include
  `<iostream>` have a 60 s timeout. Every one of them passed on a re-run. About an hour went to
  re-running and checking.
  - Do: check `uptime` first. Re-run only what timed out: `ctest --test-dir build -R '<names>'`,
    then `tools/test failed affected core` to skip the rebuild. A test that times out only under
    the driver is waiting in the daemon's queue. `YCXX_DAEMON=0 /usr/bin/time -p build/bin/ycxx …`
    shows its own CPU time.
  - Learned on #330.
  - Learned on #218.
  - The same holds for the first build of a fresh worktree. On #295, edits made while it built went
    into the binary meant to show the new tests failing. A sibling worktree's binary of `main`
    served instead: check its commit with `git merge-base --is-ancestor`, copy it to the
    scratchpad, and run it as `YCXX=<copy> tools/test …`. Two `cmake --build` runs started in
    one build directory also collide, and one stops with "subcommand failed".

## Working with Fable subagents

- **Agree the contract between parts before splitting a design.** #1's engine part and C++ part
  were briefed separately. The C++ part then needed a capability the engine part had never heard
  of (contextual queries with verifying traces). That cost a full reconciliation round, and later
  rounds found more P0s at the same seam (what a context is, what a trace holds).
  - Do: before fanning out, have one subagent write the interface between the parts (shared
    vocabulary, key shapes, what each part may assume of the other). Give it to both.
  - Learned on #1.
- **Two subagents must not edit one file at once.** Splitting a design into parts with one
  subagent each works well. Parallel edits to the same file are still risky.
  - Do: give each part its own file. On #1 the second part was extracted to a scratch file,
    edited there, and spliced back by the session. Splice after both parts are final, and relay
    decisions that cross parts through the session, explicitly.
  - Learned on #1.
- **Point subagents at files, don't paste.** Review findings, the current text of a part, and
  probe results went to subagents as files in the scratchpad, with a short brief naming which
  items are theirs. Briefs stayed small, and two agents could share one list of findings.
  - Learned on #1.
- **Probe the toolchain before briefing a design question that depends on it.** For the switch to
  coroutines and `std::execution` (DECISIONS §3.2), five minutes of GCC 16.2 probes went into
  the brief. They covered reflection-derived hashing, contracts, coroutines, `import std`, and
  the fact that libstdc++ lacks P2300 senders, which have to come from libycxx. The brief then
  rested on facts, not guesses.
  - Learned on #1.
- **Review rounds can go on indefinitely.** The query-engine design went through three review
  rounds, and each one found a new layer. Most P0s in rounds 2 and 3 were in text that the
  previous round's fixes had *added*: each fix brought in new mechanism, which had its own gaps.
  - Do: run another round while a review finds P0 or P1 issues. When a round finds only P2
    issues, fix them in the same commit and stop. Open issues for deeper questions the review
    raises, instead of reopening the design.
  - Ask for the smallest fix that closes each finding, not a redesign.
  - Have the verifier check the fixes and the diff, not the whole document again.
  - Review the assembled document as soon as it is assembled. Seams between separately written
    parts are where most P0s were.
  - Seeded on #1. Revise this entry if it proves wrong.
- **Run spikes in parallel with the design subagent, and start them first.** On #3 the spikes
  (a toy e-graph, solver builds) ran on Opus while Fable drafted. Their results changed the
  draft's numbers (exact ILP extraction stops being practical near 1,000 e-nodes, not 2,000
  nodes), so the decision subagent should have the results, or be told which claims wait on them.
  - Do: brief the decision subagent to ask for a decision record (decision, reasons, rejected
    alternatives), not prose, and to mark claims that wait on a spike. The session writes the
    prose (DECISIONS §6.5).
  - Learned on #3.

- **Ask Fable for a terse decision list, and do the writing yourself.** On #2 the first brief asked
  for a 3,000-word drafted part; mid-run it was redirected to "decision, reason, rejected
  alternatives, one or two lines each". The decision lists (about 1,700 and 2,100 words) were
  enough to write the document from, and gaps were cheaper to spot in them than in prose.
  - Do: brief Fable for numbered decisions with reasons and rejections, plus "what other parts
    must assume". Push back on gaps with a short follow-up to the same agent. Now a rule:
    DECISIONS §6.5.
  - Learned on #2.
- **Write the interface first, then one brief for the rest.** #2 gave the handle vocabulary and
  core decisions to one Fable brief, then everything else to a second brief that took the first
  as its contract. The second brief's findings against the first were all local fixes, and there
  was no reconciliation round.
  - Learned on #2.
- **Merge `main` just before the final review when sibling designs are running.** #3 was accepted
  while #2's review ran, and #2's debug-info section then had to be re-aligned after the review.
  - Do: before the one independent check, merge `main` and read any newly accepted design that
    shares a boundary, so the check covers the reconciled text.
  - Learned on #2.
- **Ask the reviewer for a failing example and the smallest fix per finding.** #2's single
  independent check found 9 P1s; every one came with an example and a local fix, so all 24
  findings were fixed in one commit and the second check covered only the diff.
  - Learned on #2.

- **A reviewer subagent in your own worktree collides with you.** On #40 the reviewer's
  temporary probe edits showed up in `git status` mid-commit, and switching branches under it
  changed what `HEAD` meant for its second pass.
  - Do: give a reviewer a commit SHA, not `HEAD`, and `isolation: "worktree"` when it may edit
    or build; otherwise don't switch branches until it reports.
  - Learned on #40.

- **Brief a plumbing design with the real invocations, not only the principles.** On #83 the
  decision brief described `make -j` in general. The one review then found a P0 and three P1s in
  invocations it never listed: recursive make running the same command line in two directories,
  a link reading `.o` files that the build keeps rewriting, `echo | c++ -E -` probes, and
  `SOURCE_DATE_EPOCH` next to clock-timed requests.
  - Do: list concrete command lines and event sequences in the brief (working directories,
    response files, stdin, environment, what the build writes where), and ask the subagent to
    walk each one through its decisions.
  - Learned on #83.

- **A subagent that writes files must not share your worktree while you switch branches.** On
  #87 the spec-only test author wrote into the session's worktree while the session parked work
  in WIP commits and switched to the PR 1 branch for review fixes. Its files were swept into a WIP
  commit, and a failed `cd` left copies at the root.
  - Do: give a writing subagent `isolation: "worktree"`, or don't switch branches until it
    reports. Before a PR, rebuild unpushed history from a soft reset, so stray WIP never ships.
  - Learned on #87.

## Bureaucracy loops

- **A design document became a loop.** The query-engine design (#1) grew to 31,500 words over
  seven rounds of pushback and three reviews, plus repeated relays between two subagents. Each
  round's fixes added text, and the next round found new problems in that text.
  - Do: write decisions, reasons and rejected alternatives, nothing more.
  - Do: settle mechanism questions in the spike, not in prose.
  - Do: stop reviewing when a round finds no P0 or P1 issues (DECISIONS §8.6).
  - Learned on #1.

## Toolchain

- **A sandbox command's `/dev/shm` is private to that command.** A compiler built there by one
  command was inaccessible to the next.
  - Do: build and test in the same command when using `/dev/shm`. Copy the binary and test logs
    to the session's workspace before the command exits if another command needs them. Keep the
    build shell alive after a failed check if a retry needs the existing build.
  - Learned on #101 and #407.

- **The managed Linux sandbox can affect daemon tests.** The default sandbox denies Unix socket
  bind or connect, and the sandbox's `/tmp/.git` placeholder can make root discovery pick
  `/tmp` for a test that expects to fall back to its home scope.
  - Run `tools/test --fast` with per-command network access and `TMPDIR=/dev/shm`.
  - Learned on #216.

- **Linux native mixed-object checks need host objects in the linker's supported ELF subset.**
  A GCC allocation provider emitted an unsupported absolute relocation, and a Clang provider
  emitted `.llvm_addrsig`. A constant pointer global also landed in GCC's `.data.rel.local`.
  - Do: build header-free peers with `-fPIE -fno-exceptions -fno-asynchronous-unwind-tables
    -fno-unwind-tables` and add `-fno-addrsig` for Clang. Form a local object's address where it
    is used. Compile a C++ peer's `main` in hosted mode to retain the ELF entry symbol.
  - Learned on #293. The wider object input work is tracked in #390.

- **x86-64 Linux runs on an arm64 Mac go through Docker's amd64 emulation.** The `gcc:16`
  image on this Mac is arm64, so CI's `linux-x86_64` job could not be reproduced locally, and
  x86-64 back-end work waited for CI round trips.
  - Do: build an image once, `FROM gcc:16` with `ninja-build cmake python3 git rsync`, with
    `docker build --platform linux/amd64`. Run it with `--platform linux/amd64`, the worktree
    mounted read-only and a named volume for the build. In the container, `rsync -rlD --checksum --delete
    --exclude /build --exclude .git` the tree into the volume (not `-a`: kept modification
    times older than the build's outputs make Ninja skip changed files when you switch
    worktrees), then run `cmake -S . -B
    /vol/build -G Ninja` and `cmake --build`, not `tools/build`, whose submodule sync cannot see a
    worktree's `.git`. Run CTest fixtures (`ctest -R unit.linux_…`) there. The first build
    took about 25 minutes under Rosetta, and incremental ones take a few.
  - Do: the test driver calls ycxx without `--target`, and a Linux host still defaults to
    Darwin. Until the driver passes the target, run the own run suite through a wrapper that adds
    `--target=x86_64-pc-linux-gnu -nostdlib` (#646).
  - Learned on #639.
- **Survey a Release build's warnings in one pass, then fix them.** The first Release build
  (#726) stopped at each `-Werror` failure, and every rebuild under load took 20-40 minutes to
  find the next one.
  - Do: build once into a separate directory with
    `-DCMAKE_CXX_FLAGS="-Wno-error=maybe-uninitialized -Wno-error=array-bounds -Wno-error=stringop-overflow -Wno-error=stringop-overread"`
    and `ninja -k 0`, then list every warning site with its `inlined from … at src/…` chain.
    That build found 9 sites in all; fix them, then build with `-Werror` again.
  - GCC 16.2's false positives at -O3 come in three kinds: `std::optional`'s payload read after
    a check (`-Wmaybe-uninitialized`), and libycxx's in-place `vector`/`string` insert and
    `memchr` paths (`-Warray-bounds`, `-Wstringop-*`; yrashk/libycxx#51). The fix that held was
    a value and a flag instead of the optional. Copying `*opt` into a local after the check did
    not help.
  - Debug hides what -O3 shows: a null `Json*` dereferenced by a test returned null at -O0
    (`unit.lsp`), and a daemon test's edit window, sized for Debug's compile time, closed in
    Release. `daemon.process` now parks the pooled build at a checkpoint (#746). A Release-only
    test failure is a test bug as often as a compiler one.
  - A GCC crash or link failure that a guessed repro misses: copy the intermediate modules
    verbatim and reduce them. #283's trigger was an inline constructor in an *intermediate*
    module, and #46's was a template specialization instantiated in the imported interface.
  - Learned on #726.
- **A module import added during a build can miss the dependency scan.** CMake scanned a test's
  imports before an edit and GCC compiled it afterward, so its module mapper lacked the new import.
  - Do: rerun Ninja in the same build directory after changing imports during a build.
  - Learned on #101.

- **Find where an incremental build's time goes with the trace's counts and a sampler's times.**
  On #938 the JSON trace of a 1 MB file had 280k events and doubled the build. Its per-kind self
  times blamed `FunctionSourceQ` for 2 s of CPU after its scan was already gone, because each
  traced cell pays the tracer's own cost. The development machine also runs other sessions'
  builds (load 11–17 on 18 cores), so single timings varied by 50%.
  - Do: take outcome counts per kind from `--trace-engine=json` (what re-executes, and why). Take
    times from `sample <daemon pid> 3` started just before an untraced rebuild, and from wall
    times: min of 5 runs, alternating the base and changed binaries (copy each to a path of its
    own; a daemon is per binary). Check `ycxx ctl status` for memory without the trace's events.
  - Look for O(n) scans of unit-wide values in per-function cells: each looks cheap in a small
    test and is quadratic over a large unit.
  - Learned on #938.
- **Find analysis-order dependence with an oracle before splitting analysis.** On #947 a debug
  check analysed each top-level item in a `parse::Program` of its own and compared its results
  with the whole unit's, over the core suite. It found 32 differences in 23 files: side tables
  filled as items run, read on the assumption that every earlier item had run. Fixing their
  causes took an afternoon; finding them by debugging failing groups would have taken days.
  - Do: before running part of an analysis alone, compare part-alone with whole over the suite,
    and fix each cause where the table is read (analyse what can fill it first).
  - Learned on #947.
- **The oracle finds only what the suite exercises; list per-unit state by hand too.** After #987
  merged, source review found two regressions the oracle and 440 fuzz rounds had not. A typedef's
  `aligned` attribute marked its class's layout uncertain only in the group that saw the typedef,
  and the merge kept the defining group's shape (#992). The once-per-unit template-depth message
  guard reset per group, so two families reported twice (#993). No suite file had either case.
  - Do: when splitting an analysis, list every member that is once per unit (flags, counters) or
    that a later item can change for an earlier entity, and write each one's merge rule down
    (union, first-wins, once), with a test for each that is not first-wins.
  - Learned on #947.
- **Record a recorded question before its loop.** #947's by-name questions were recorded per
  entry they visited. When no item declared the name yet, nothing was recorded, and a group
  missed the declaration added later. The `.incr` suite caught it.
  - Do: record the question, then walk its answer; check a reuse test by disabling one part of
    the verification and seeing the test fail.
  - Learned on #947.
- **GCC 16.2's "Bad file data" can come from a partition's non-exported code too.** On #947, new
  types and code in `src/pipeline/front.cppm` made every importer of `ycxx.pipeline` fail, exported
  or not. Moving them into a module implementation unit (`src/pipeline/groups.cpp`, `module
  ycxx.pipeline; import :front;`, as `src/interp/ops.cpp` does) fixed it.
  - Do: put implementation that importers need not see in an implementation unit.
  - Learned on #947.
- **Template code added to the engine can slow its hit paths without touching them.** #245's
  trace hooks were one branch each, yet `engine_bench` lost 6 ns a fetch hit and 6 ns a root get
  hit. The per-kind `describe` templates instantiated `std::format` in every engine user's unit,
  and GCC then stopped inlining `Hasher::uint` into `cell_for` (it appeared as `.isra.0` calls).
  `inline`, `noinline` on the hooks and forced alignment changed nothing; moving the formatting
  out of line did.
  - Do: for engine changes, compare `engine_bench` in Release against the base commit, taking the
    minimum of ten interleaved runs (the machine is shared). To isolate, build the base's file in
    your build directory. To find the cause, look for calls in the hot function that the base
    inlined (`objdump --disassemble-symbols=…`, then grep `bl`).
  - Do: keep per-kind template code small; put formatting and strings in non-template functions.
  - Learned on #245.
- **`await_resume` runs on the hit path too.** `co_await x` calls `x.await_resume()` even when
  `await_ready()` returned true, so a store put there "where the body resumes" lands on every
  fetch hit. On #274 a memory target set there would have taxed the 41 ns fetch hit; Fable's
  review caught it. Every resumption after a real wait already went through `Settle::resume`.
  - Do: put per-resumption work where the suspension ends (the task that resumes the frame), not
    in the awaitable's `await_resume`.
  - Learned on #274.
- **macOS's `poll` does not support devices.** It flags `/dev/tty` `POLLNVAL` at once, so a TUI
  that polled the terminal and a socket fell into a blocking read and saw the socket only after
  a key.
  - Do: wait on a terminal with `select`. Drive a TUI in tests with Python's `pty.fork`, a set
    window size, and keys written with pauses; the last frame is after the last `ESC [ H`.
  - Learned on #245.

- **Test a live, incremental model against its batch twin on simulated streams, in every order.**
  #939's live trace tree must end equal to the tree the collector's batches build, whatever order
  events come in.
  - Do: generate engine-like event streams (frames with starts and ends, runs of alike instants).
    Feed each to the real batch path and to the live path, in the engine's order, shuffled and
    backwards, in random chunks. After every chunk, check the incremental bookkeeping (sums,
    depths, folding, the summary) against a recount. At the end, compare everything the view
    shows. Unit tests written by hand missed every bug this found: sums stopping at a subtree
    waiting for its parent, and two folding cases.
  - Do: make the out-of-order path as cheap as the in-order one. Workers' events reach a shared
    buffer slightly out of id order all the time. A whole-parent refold per such event was
    quadratic under a parent of 80k children, and the browser fell 2 s behind a 2 s build. Keep the
    repair local, around the change.
  - Do: guard an index into a container that can be replaced with a contract. A stale index read
    zeroed memory, landed on row 0 by chance, and the mutation test stayed green until
    `contract_assert` made it abort.
  - Learned on #939.

- **Test a terminal UI as a model fed events, and only the terminal around it under a pty.** The
  trace browser's keys went untested while it read `/dev/tty` itself (#245). Moving its state and
  drawing into a model that takes key and mouse events and returns frames of a given size (#635)
  let every click, drag and selection be a unit test against plain-text goldens. A pty test then
  covers only what the terminal does: modes, signals, resize, escape sequences.
  - On macOS a pty's slave is revoked when its session leader exits, so the parent cannot read
    the restored modes. Make the session leader a small process that runs the program as its
    child and prints the modes once that child has died.
  - Key a selection by the cells it covers, never by row numbers: a live feed, a sort or a
    filter moves the rows under it (#636's one P1).
  - Learned on #635 and #636.
- **Show a terminal UI's change as pictures, rendered from a pty.** Polishing the trace browser's
  layout (#1119) needed screenshots at fixed sizes, before and after, for the user to steer.
  - Do: run the program under Python's `pty.fork` with `TIOCSWINSZ` set, send keys with pauses,
    feed the output to `pyte`, and draw its cells with Pillow. Feed only the last frame (from
    the last `ESC [ 1 ; 1 H`); a live build draws thousands, and pyte took minutes on all of them.
    pyte ignores SGR 2 (faint), so map it to an attribute the program never uses before feeding.
  - Copy a rebuilt binary to a new path (`rm` first). macOS kills a binary overwritten in place
    with `SIGKILL` and no output, which looks like a crash.
  - Give a scratch folder a `.git`, and stop its daemon when you replace the binary. The daemon's
    root is the nearest `.git`, so a folder under `/tmp` without one was rooted at `$HOME`, and
    runs hung behind stale daemons of replaced binaries until they were killed.
  - Learned on #1119.
- **Reduce a compiler crash from the build system's exact command, with a script.** A GCC ICE
  under CMake seemed to vanish when re-run by hand. Two things cost about 20 minutes of
  bisecting:
  - The tool shell is zsh, which does not word-split `$CXX file.cpp`. The command never ran, and
    `grep -c internal` printed 0.
  - Importing a BMI with a different `-freflection` setting crashes too, but in a different way.
  - Do: get the command with `ninja -t commands <target>`.
  - Do: drop flags and source lines one at a time in a Python script (`subprocess` with a list).
  - Do: check the exit status of the reduced command, not only the absence of an error.
  - Do: once it reproduces, cut it down to a standalone repro with no libycxx. That repro is the
    bug report, and it shows where the fix belongs.
  - Learned on #4.

- **A contract that fails while its condition holds: suspect the compiler before the code.**
  On #32, a `contract_assert` inside a lambda in a module template reported false while the same
  expression, printed or stored in a local, was true. About 30 minutes went into checking the
  interner before the predicate itself was suspected. Two standalone repros did not reproduce it.
  - Do: when a contract fails against values that look right, evaluate the predicate into a local
    `bool` first. If that passes, it is the compiler: apply that as the `// BOOTSTRAP:`
    workaround and list it in STATUS.
  - Do: print with `std::println(std::cerr, …)` inside the failing function. lldb on macOS showed
    only the `__pthread_kill` frame for a contract abort.
  - Do: print from inside the predicate itself, with a comma expression such as
    `contract_assert((std::println("{}", (void*)this), pred))`. That showed GCC takes `this` to be
    the closure, and the repro was then three lines. The two guessed repros had failed because
    neither predicate named a member.
  - Learned on #32.
- **GCC 16.2 module bugs that fail silently or late.** A defaulted friend `operator==` in an
  exported class segfaults the importer; `pre` on a function template is silently dropped when
  the template is instantiated through an import. Both are in STATUS's workaround list.
  - Do: test that a contract fires (a violation-handler test, as `unit.engine_contract`), not
    only that it compiles. Reduce a module ICE to two files with `g++-16 -fmodules` first.
  - Learned on #14.

- **A class defined inside a coroutine template's body breaks the build** when an object of it
  lives across a `co_await` (an awaitable): GCC 16.2 stores it in the frame type and reports
  `-Werror=subobject-linkage` ("has a field whose type has no linkage") in every importer.
  - Do: define awaitables at namespace scope, as class templates if they need the query's types.
  - Learned on #15.
- **The system linker on a dev Mac can be newer than CI's.** Weak Mach-O definitions in
  `__DATA,__bss` coalesced under macOS 26's linker, but macOS 15's (the `macos-15` runner)
  reported them as duplicate symbols. Clang emits weak zero-initialized data in `__DATA,__data`.
  - Do: when ycxx's objects go through the system linker, emit what Clang emits for the same
    construct (`clang++ -c`, then `nm -m` and `otool -lv`), not just what the local linker accepts.
  - Learned on #124.
- **A zero-fill word that changes with no store: compare it with the file.** On #842 a `__bss`
  word of a ycxx-linked program held garbage by `main`, and lldb write watchpoints never fired.
  The value was the file's bytes at the segment's `fileoff` plus the word's offset (an `nlist`
  entry of `__LINKEDIT`): the chained fixups listed the zero-fill pages, and dyld's page-in
  linking maps every listed page from the file, in the kernel. It also depended on member order,
  since a set-ordered member list moved the word.
  - Do: when a value appears without a store, `xxd -s <fileoff + offset>` the file there first,
    and diff `dyld_info -fixup_chains` against the system linker's output for the same objects.
  - Learned on #842.
- **An executable killed at exec with status 137 says nothing about why.** A linked Mach-O whose
  `ncmds` was one too high passed `codesign -v`, then the kernel killed it before `main`.
  - Do: run `otool -l` on the output first. It reports "malformed load command" and where.
    `dyld_info -fixups -exports -imports` checks chained fixups and the exports trie the same way.
  - Learned on #42.
- **GCC cannot parse `<CoreServices/CoreServices.h>`**: Apple's headers declare blocks (`^`),
  which GCC 16 rejects. FSEvents alone is plain C.
  - Do: `#include <FSEvents/FSEvents.h>` in the global module fragment, with
    `-F<sdk>/System/Library/Frameworks/CoreServices.framework/Frameworks` on that one file, and
    link `-framework CoreServices` (src/daemon/CMakeLists.txt). `<dispatch/dispatch.h>` parses.
  - Do: platform code goes in one module partition per platform, picked by CMake (`:os`,
    `:watch` in src/daemon); the preprocessor policy rules out `#if`.
  - Do: check Linux-only code before pushing, with the CI image:
    `docker run --rm -v "$PWD":/src:ro gcc:16 …`, copying the tree and running cmake and ctest
    directly (`tools/sync-submodules` cannot see a worktree's `.git` from inside the container).
  - Learned on #96/#97.
- **A watcher test that passes at once may not test the race.** The FSEvents test of "an edit
  between the read and the new stream" passed with the replay removed: a stream created "since
  now" still saw an event `fseventsd` had not journaled yet.
  - Do: mutate the code under test, and add the delay that makes the event happen before the race
    window (here, 1 s before recreating the stream).
  - Rule: DECISIONS §5.6.
  - Learned on #96/#97.
- **libycxx deprecates `std::filesystem::path::string()`** (C++26 [depr.fs.path.obs]), and
  `-Werror` turns it into a build failure.
  - Do: use `native_encoded_string()` for file names passed on, `display_string()` for messages.
  - Learned on #42.

- **A contract predicate sees the parameters as `const`** ([dcl.contract.func], P2900's implicit
  const-ification). A `pre` that called `is_void(Interners&, …)` did not compile.
  - Do: give predicate helpers `const&` parameters, and answer them from data that is already
    computed (`types.canon(q)`), not from functions that may intern.
  - Learned on #33.
- **Undo a mutation with the inverse edit, not `git checkout <file>`.** On #337 a mutation was
  reverted with `git checkout src/interp/ops.cpp`, which also dropped the uncommitted fix it was
  testing; only `git status` afterwards showed it.
  - Do: commit (or stash with a tag) before mutating, or undo with the same `sed` reversed, and
    check `git diff --stat` after.
  - Learned on #337.
- **A mutation that does not build leaves the old binary under test.** On #647 a mutation
  left a variable unused, `-Werror` stopped the build, and the test passed against the
  previous, correct binary. The "surviving" mutation looked like a test gap.
  - Do: check the build's exit status, not just the test's, after every mutation, and keep
    the mutated code warning-free (`+ 0 * x` keeps a variable used).
  - Learned on #647.
- **A mutation that changes nothing proves nothing.** On #33 the first mutation of
  `usual_arithmetic` (`>=` to `>` on rank) was equivalent for the standard integer types, and the
  suite rightly stayed green.
  - Do: mutate a branch that some test input must take, and check that the suite turns red.
  - Rule: DECISIONS §5.6.
  - Learned on #33.

- **Linux x86-64 run tests can be checked from the Mac without building ycxx in Docker.** Compile
  with the Mac's ycxx (`ycxx cxx --target=x86_64-pc-linux-gnu -c`), then link and run with `g++`
  in the `ycxx-linux-amd64` image (`docker run --platform linux/amd64 -v dir:/w`). On #309 this
  ran 13 mixed-object tests in about a minute, against 25 minutes for an emulated build.
  - Learned on #309.

## Data the front end needs

- **Deferred analysis reads committed parameter lists.** On #713, the exception-specification
  replay read a declaration copied by `Program::declared` before its item committed. A draft
  `SeqId<ParamSema>` happened to name a different parameter list in the global interner.
  - Do: read the committed member item for the relevant class specialization before using the
    global sequence interner. Check a parameter reference independently in the pattern and its
    replay when a name is found in the definition and lost after substitution.
  - Hit again on #828: a new pattern check in `declare` read `sem.params`, a draft sequence, and
    crashed. Pass the item's own `params` vector instead.
- **A function template specialization's identity includes its type.** `Sema::specialization`
  keys entities by template, arguments and type. On #828, the address path re-created a
  specialization with its resolved `noexcept` type while the call path kept the written one: one
  function became two (a split static local, or a duplicate symbol), with no diagnostic.
  - Do: resolve a specialization's exception specification on every path that selects it (call,
    member call, address), and only after selection ([over.over], [except.spec]/13); test a
    specialization that is both called and has its address taken, with a static local.

- **Unicode tables from Python, plus one UCD file.** Python's `unicodedata` (Unicode 17.0 in the
  pinned Python) gives categories, combining classes, decompositions, normalization and names;
  `str.isidentifier` implements XID_Start/XID_Continue. It cannot enumerate name aliases. The
  first version took them from the host's Perl (Unicode 13): 13 were missing, Linux had none,
  and a review caught it with the missing license notice.
  - Do: vendor the UCD file of the same version (`tools/unicode/ucd/`) with the Unicode License
    v3 (`src/lex/LICENSE.unicode`), and have the generator check the versions agree.
  - Learned on #40.

- **`git pull` leaves submodules where they were.** After a libycxx bump, a checkout whose
  `deps/libycxx` was not updated built the old library and hit a GCC ICE that the bump had fixed.
  - Do: `tools/build` and `tools/test` run `tools/sync-submodules` first, so the pin is enforced
    rather than remembered.
  - Learned on the α slice (#39), 2026-11-18.

- **A helper whose "no" also means "not modelled" misleads its next caller.**
  `unique_public_base` returned null both for "no unique public base" and for "a polymorphic
  class, which the value conversions do not model". On #218 pointer convertibility read the
  null as "no", so `std::derived_from` was false for every polymorphic hierarchy (the review's
  P1).
  - Do: give a bounded query a tri-state result (`yes`, `no`, `unknown`), as `base_path` and
    `PointerConversion` now do, and let each caller decide what `unknown` means.
  - Learned on #218.

## Evidence and tests

- **Map a header chain past its first wall with an overlay, and check both gates.** Every
  header on `<iostream>`'s path stopped at one line of `move.hpp`, which showed nothing behind it.
  - Do: copy only the blocking libycxx file into a scratch directory, stub the construct there,
    and put that directory first with `-isystem`. A copy of the whole include tree breaks
    `#include_next`.
  - Do: run `-c` as well as `-fsyntax-only`. The mangler runs only for objects, so the first
    `-c` error (#773) never showed in a syntax sweep.
  - Do: reduce each wall to a header-free example and check it with Clang and GCC before filing.
    Clang and GCC disagreed on the mangling there (#777), which made it a decision, not a fix.
  - Learned on #776.

- **A feature probe must use nothing but the feature.** On #282, 40 of the first 150 probes failed
  on something else: `auto` to hold a lambda, a nested `struct`, a default argument, `alignas`.
  Each would have filed the wrong status and the wrong failing example.
  - Do: write a probe from features already known to work, and confirm each failure with a
    one-line micro-probe of the feature alone before recording it. ycxx's first error names the
    construct it stopped at, which shows at once whether that is the feature under test.
  - The same pass found that a coverage check walking up to cited ancestors could not fail
    (`[class]` covered every `[class.*]`): mutate the data (rename a cited clause) to see the
    check fail (DECISIONS §5.6).
  - Learned on #282.

- **Start the spec-only test author at the slice's first commit, briefed on every step.** On #336
  the author wrote 84 tests from the draft while the back end was being written, checked against
  both oracles. Run against ycxx they found two front-end bugs no hand probe had (arithmetic
  between an enumeration and a floating type accepted; `static_cast<E>(-2.9)` not constant), a
  lexer error the draft's own example contradicts (an overflowing literal is an infinity), and
  that `<math.h>` is libycxx's wrapper of `<cmath>`, so no `<math.h>` test can pass until #349.
  - Do: launch the author in its own worktree before writing the back end; cherry-pick its commit
    and triage each failure as a bug, a missing feature (XFAIL with the issue) or a test that leans
    on an unrelated gap (move that line into a function).
  - Learned on #336.
- **Test a long-lived connection's lifetime before the review.** Both P0s of #99 were in the
  LSP tunnel, and the functional tests could not have shown either:
  - a one-thread copy loop deadlocked once a large answer and a large edit crossed;
  - an open session kept the daemon from ever shutting down.
  The review's probes found both in minutes.
  - Do: for any connection that outlives a request (a tunnel, a watch, a subscription), write
    two tests first. One sends messages larger than the socket buffers both ways at once. The
    other stops the server while the connection is open and checks that both ends exit.
  - Learned on #99.

- **Build twice in one session before calling a stage incremental.** On #63 the first warm
  rebuild crashed in sema: `Entities` assumed it allocated every entity handle, and the back end
  had interned string literal entities in between. Nothing per-process had shown it.
  - Do: test a cell graph with a session that builds, edits and builds again, and compare with a
    cold build. Insert a declaration first too: the warm session then numbers entities
    differently, which catches output ordered by handle.
  - Learned on #63.

- **Exercise reuse through the real driver, with several translation units.** In each of #85's
  three parts, the one review found an unsound reuse that `unit.pp` missed:
  - a main file and a header shared a region;
  - the include depth was not part of a region's value;
  - an include guard's empty region dropped directives outside the group.
  All three showed with `ycxx cxx -E a.cpp b.cpp`, whose TUs share one session. The unit test's
  edit sequences varied the files, but not the roots, the depths or what lay outside a guard.
  - Do: for a cache keyed by something less than all the inputs, list what the value also depends
    on (root or not, depth, text outside a summarized part). Then write one multi-TU repro per
    item, through the driver, before the review.
  - Learned on #85.
- **A spec-only test author keeps the oracles honest.** Asked for "a skipped group may hold an
  unmatched `'`", #85's test author found that [lex.pptoken]/2 makes it ill-formed. GCC agrees,
  and Clang, which ycxx had copied, does not.
  - Do: brief the author with the clauses, not with the expected behaviour, and treat an
    oracle-versus-draft disagreement it reports as a decision to make, not a test to drop.
  - Learned on #85.
- **An incremental-equals-cold check also catches values that depend on the session.** On #85,
  `unit.pp` compared a warm session's `Region` with a fresh one's after each edit. Five states
  differed, while every token, location and name matched. The difference was `Region::files`,
  which held `FileId`s numbered in the order a session first saw each path.
  - Do: compare whole values, not what they print. Keep handles that a session numbers out of
    values; use paths or fingerprints.
  - Learned on #85.

- **Rerun the crash survey after every change, through the real driver.** On #41 a survey of
  every draft example found no crash; two later changes (placeholder records with an uninterned
  type, then the switch to #33's checked `array_of`) brought crashes that the earlier survey
  could not see, and test flags (`-freflection`) brought exit 2s that only the driver passes.
  - Do: keep the survey a loop over `tools/test core` plus a direct run, and rerun it last.
  - Do: an expected-failure list must never cover a crash (`Crashed` in tests/driver/kinds.py).
  - Learned on #41.
- **A default-constructed handle is a real handle.** `QualType{}` is type number 0, the first type
  interned (often `int`). The interpreter used it as "no pointee" for `std::nullptr_t`, and so
  skipped its `void*` cast check for every `int*`. The unit tests' world happened to intern `int`
  first, which is how it showed.
  The same happened three times on #88: `EntityId{}` in a new `TemplateArg::templ` field made every
  dependent template argument name entity 0 (the first class template declared), `ConstId{}` did
  the same in step 6, and an empty `SeqId<TemplateParamSema>` crashed until list 0 was interned as
  the empty one.
  - Do: say "none" with a flag or `std::optional`, or with the kind's `none` enumerator where the
    handle has one (`ConstId::none`, `NodeId::none`), never with `T{}`. Give a new handle field
    `= X::none` (or intern the empty list as index 0), and test with the argument declared first.
  - Rule: AGENTS.md, "While working".
  - Learned on #25 and #88 steps 6 and 8.
- **Test blocked storage past its first block.** #107's chunked pools passed a concurrency test
  that never left block 0; the review found that any list longer than the next block aborted,
  which one 3,000-statement function triggers.
  - Do: test such storage at the block edges, with a range longer than a block, and at a scale
    where every column crosses several blocks.
  - Learned on #107.
- **A reference into a growing side table dangles.** A `const TypeRec&` held across creating
  another type pointed into a reallocated `std::vector`, giving a wrong diagnostic that depended
  on the file's length. Use a `std::deque` for tables whose records are referenced, or return
  records by value.
  - Learned on #41.

- **A check over a build that reuses everything checks nothing about work.** On #274 the JSON
  memory checks ran on a build after an unchanged rebuild. Every event was `reused` and every sum
  was 0 = 0, so dropping the per-kind sums went unnoticed until the mutation run.
  - Do: run checks about what work did (sums, allocations, executions) on a build that re-runs
    something, and assert the total is non-zero.
  - Learned on #274.
- **Fuzz incremental == cold on every derived output, diagnostics included.** The engine's
  values, dependency lists and executions matched cold runs from the start, but `diags_of` was
  wrong in 119 of 300 edits: pruning by `has_diags` missed a backdated cell whose subgraph gained
  diagnostics, and a backdated cell whose dependency list changed. The design had argued the
  pruning in prose. A mutation check (drop one recorded dependency) confirmed the fuzz bites.
  - Do: compare every output kind against a fresh `Db`, and check "nothing executes" for every
    cell the cold run executed. Mutate the engine once to see the check fail.
  - The review then found what the fuzz missed: a cell validated green kept a stale durability,
    because durability is a derived field outside the fingerprint, so the shortcut served a wrong
    value. Any cached field that is not in the fingerprint needs its own update rule and test.
  - Learned on #14.

- **A `std::tuple` over a long type list is a compile-time trap with GCC 16.** The AST spike stored
  240 node kinds as `std::tuple<std::vector<Ts>...>`; `std::get` on it cost over 50 s of front-end
  time, while the reflection-derived code for all 240 kinds took about 2 s.
  - Do: index per-kind storage by a kind number (an array of untyped pools), and measure
    `-ftime-report` before blaming reflection.
  - Learned on #2's spike.

- **Check generated code against C, not against expectations you wrote.** The back end's first
  plan widened every narrow argument to 32 bits in `lower`. Apple arm64 does that only for
  register arguments; on the stack a `char` keeps one byte. That came out of rereading Apple's ABI
  notes, not out of a test, and the plan's own golden tests would have agreed with the mistake.
  - Do: link generated functions with a C harness compiled by the bootstrap compiler. The harness
    computes the expected value with the same C expression and calls across the ABI both ways, with
    more than 8 arguments and narrow types. One harness covered about 60 functions and thousands
    of value pairs.
  - Do: mutate the back end once (swap a pattern's form, flip a calling-convention flag) and see
    the harness fail.
  - Learned on #26/#28.
- **Test a selection's invariant end to end, not just its helper.** The test driver's shard
  helper was unit-tested and correct, but main.py fed each stage a list that depended on earlier
  shards' picks, so shards skipped some tests and doubled others: CI could go green without
  running them. A review found it; a self-test running `tools/test --list --shard i/N` for every
  i and checking the union equals the unsharded list now guards it.
  - Do: for partitioning, ordering or filtering code, check the invariant (every item once,
    nothing lost) through the real entry point, with state that makes the stages interact.
  - Learned on #5.
- **CTest fixtures can also be discovered by `tools/test`.** The affected stage recognized
  `.accept.cpp`, `.reject.cpp` and `.run.cpp` under `tests/unit/` and ran Linux fixtures with the
  default Darwin target, after their Linux CTest passed.
  - Do: give CTest-only C++ fixtures plain `.cpp` names and check discovery before the full gate.
  - Learned on #196.

- **A harness check must be able to fail: mutate where the data actually flows.** On #19, a
  mutation dropping the engine's file-input dependencies did not bite, because headers reach the
  pipeline through pp's session and the daemon's file table. The review then found three checks
  that could not fail: a restarted daemon answers like a cold one, `-E` runs no engine query, and
  the comparison dropped messages.
  - Do: for each check, name the bug it catches and mutate that layer (here the daemon's batch,
    a daemon that dies); compare everything the compiler writes, not a projection of it.
  - Rule: DECISIONS §5.6.
  - Learned on #19.
- **A verifier's tests need malformed inputs, not only round trips.** #23's round-trip tests passed
  every op, and the one review still found two P1s in the verifier: a terminator's results accepted
  on a second path into the normal successor, and an out-of-bounds read on a malformed body. Each
  showed up with a five-line hand-written body.
  - Do: for every rule a verifier enforces, write one body that breaks it and check the error. Do
    the same for every guard the parser relies on.
  - Rule: DECISIONS §5.6.
  - Learned on #23.

- **Let the oracles' output drive bulk-classification rules.** The draft-example extractor's first
  rules for "OK", "error" and "fragment" were guesses. Counting the oracles' error messages and
  the comments on the lines they hit found the real annotation forms ("ambiguous", "access
  error") and fragment signatures in three rounds, and halved the noise in the disagreement table.
  Running the generated suite through `tools/test --cxx clang|gcc` then caught 25 tests whose
  markers the driver did not recognise, which the extractor's own comparison had missed.
  - Do: write the crudest rules, run the oracles, histogram the messages, then refine. Check the
    generated tests through the real driver before committing them.
  - Learned on #6.

- **Run sanitizers on any concurrent code before trusting its results.** The spike's fork-join
  passed its demos while one of them crashed now and then: the joiner freed the join while the
  last finisher was still notifying it. ASan found the bug at once; TSan and repeated fuzzing
  confirmed the fix.
  - Do: build `-fsanitize=address,undefined` and `-fsanitize=thread` variants of concurrent
    code, and run them before quoting any numbers.
  - Learned on #1's spike.

- **Determinism checked only on cold builds proves little.** In a cold build every result is new,
  so the outcome at 1 and N threads is equal by construction.
  - Do: compare thread counts on incremental rebuilds after edits, where early cutoff and reuse
    actually happen.
  - Learned on #1's prototype review.
- **A build script can report PASS for a crash.** In bash, `if run; then` disables `set -e` inside
  `run`, so a failing step in the middle went unnoticed and a segfaulting smoke test passed.
  - Do: run the step in a subshell, `if (set -e; run); then`, and check the script once against
    a deliberately failing step.
  - Learned on #3's solver spike.
- **GCC has no ThreadSanitizer on macOS arm64.** A TSan build needs Clang and libc++, and so does
  every C++ library it links (the e-graph spike needed a second HiGHS build).
  - Do: plan TSan runs on macOS with Clang from the start, or run them on Linux.
  - Learned on #3's e-graph spike.
- **ycxx's ThreadSanitizer build runs on a Mac in the `gcc:16` image.** The arm64 image's GCC
  16.2 ships libtsan, and Docker Desktop's VM has `vm.mmap_rnd_bits` at 18, under TSan's limit of
  28. Install cmake and ninja-build, mount the checkout, keep the build in a Docker volume, and run
  `YCXX_TSAN_BUILD_DIR=/build tools/tsan`. It took about 25 minutes. The CI job's first run took 47 (17 of them
  building GCC on the cache miss), and a dispatch round trip costs more than a local rerun.
  - Do: run tools/tsan this way before dispatching the nightly job. A canary keeps a clean run
    honest: reintroduce a known race (here, the deque's fence-based push) and see the test fail.
  - Learned on #105.
- **GCC 16.2 can write a module interface it cannot read back.** Adding a member of a new struct
  type to `pipeline::TranslationUnit` (a reflection-serialized engine value) made every importer of
  `ycxx.pipeline` fail with `failed to read compiled module cluster N: Bad file data` and `failed
  to load pendings for 'std::__y1::allocator'`. Deleting the `.gcm` and rebuilding writes the same
  broken file; `std::map` for `std::unordered_map` and moving coroutine bodies out of line did not
  help. The same struct as a member of another value (`FrontResult`) was fine.
  - Do: when this message appears after an edit to a module interface, bisect the edit (comment
    out the new member and its uses) instead of rebuilding; move the member to another type.
  - Learned on #115.
- **Item ids hash the syntactic scope, so two classes of one name share their members' ids.**
  `Program::index(ItemId)` maps an id back to an item. `struct Kind<T*> { int get() const; }` and
  the primary `struct Kind { int get() const; }` gave both `get` items one id, and instantiating
  from the partial specialization used the primary's member, then crashed in `std::map::at`.
  - Do: when a construct adds another definition of an existing name (specializations, and
    later redeclarations of anything that has members), check that its member items' scope differs.
    The splitter now scopes a class's members by its template-id (`class_scope`).
  - Learned on #88 step 3.
- **A probe script run from the worktree writes its files into the commit.** An audit script
  that wrote its cases to relative paths was run with the worktree as the working directory.
  `git add -A` then committed 39 probe `.cpp` files at the repository root, and only the review
  caught it.
  - Do: write probe and scratch files under the session's scratchpad by absolute path, or `cd`
    into it in the same command. Before committing, check `git status --short` for files you did
    not mean to add.
  - Learned on #212.
- **"Not supported yet" on malformed input reads as "the feature is missing".** A class missing
  its `;` swallowed the next declaration and was reported as "'struct' is not supported yet",
  and the user concluded classes did not work.
  - Do: when a feature lands, feed it the common mistakes (a missing `;`, `}`, `)`, `:` or name,
    a stray token). Check that each gives a syntax error on the line Clang and GCC use, not a
    not-supported message or silent acceptance (`tests/own/syntax`).
  - Learned on #212.
- **Template parameters are positional, so "the current instantiation" is a property of the
  point, not of the type.** `R<T>` written inside an alias template `template<class T> using RT =
  typename R<T>::type;` is the same entity as `R`'s own current instantiation, because both `T`s are
  (depth 0, index 0). Looking members up early in "any class that is a current instantiation"
  read the primary template's member where a partial specialization's was meant.
  - Do: look a dependent class's members up early only when it is the class whose
    member-specification is being read (`Sema::current_class`), or when matching an out-of-class
    member definition against its own class.
  - Learned on #88 step 5.
- **The item splitter must learn every token-level construct that looks like `name(`.** It decides
  what a namespace-scope item declares from tokens alone, and a miss is silent: `typedef
  __remove_cv(const int) T;` was split as a function named `__remove_cv`, so `T` never existed and
  only its uses failed, far away.
  - Do: when adding a specifier that takes parentheses (`decltype`, `__attribute__`, the type-trait
    built-ins), teach `items.cppm`'s declarator scan and its function detection, and test a
    namespace-scope typedef, variable and function template whose type begins with it.
  - Learned on #211.
- **The Clang oracle is upstream Clang (the driver's toolchain), not `/usr/bin/clang++`.** Apple
  Clang and upstream Clang differ in ABI corners: on arm64 macOS, Apple Clang 21 passes a class
  with a user-provided move constructor in registers, while Clang 23 and GCC pass it by address
  (#230). Mixed-object checks done by hand against Apple Clang all passed, then the `PEER:` test
  crashed under the driver.
  - Do: for ABI checks, compile with the driver's Clang (`tools/test` prints its path; see
    `tests/driver/kinds.py`), and try GCC too. When two compilers disagree, cross-link their
    objects to find out which convention each one uses, and ask for a decision (#230 chose the
    oracles by default, with `-fclass-call-abi=apple` for Apple Clang).
  - Learned on #116.
- **A recovery loop that may consume nothing hangs only on real headers.** The requires-expression
  loop skipped to `;` after a failed requirement; at a `}` that consumed nothing, so 17 draft
  tests that include libycxx timed out while every small test passed. The next symptom, a 2×
  slowdown on the same tests, came from the new code asking the stand-in's linear lookup twice as
  often, not from the new code itself.
  - Do: give every parse loop a progress guard (`if (i_ == before) bump();`). On a timeout, run
    the one test under `perl -e 'alarm 20; exec @ARGV'`, then `sample` it: the hot frames name the
    loop or the lookup at once. Compare against main's binary before optimizing the new code.
  - Learned on #88 step 6.
- **A new `case` inserted above a fallthrough group joins it.** `case Tk::kw_static_cast:` was
  added under `float_lit`, `[`, `operator` and `~`, which had fallen through to "not supported";
  `1.0` then said "expected '<'", and calls with a floating argument failed far from the cause.
  - Do: give a new case its own body, and read the cases above it before inserting.
  - Learned on #88 step 6.
- **"As other compilers do" is a claim to check against the oracles before coding it.** #229's
  decision said to drop system headers' warnings "as other compilers do". The first version
  dropped `#warning` too, and its tests failed with Clang and GCC as the compiler under test:
  both report `#warning` from a system header (`-W#warnings`, `-Wcpp`).
  - Do: when a decision names other compilers' behaviour, write the test first and run it with
    `tools/test --cxx clang` and `--cxx gcc`, then implement what they do.
  - Learned on #229.
- **`ycxx cxx` runs through a resident daemon, which outlives a rebuild.** After #139 made the daemon
  the default, a hand-run `ycxx cxx` after `cmake --build` could be answered by a daemon started
  from the previous binary, and a debug print went to the daemon's stderr, not the terminal: a
  trace that never fired looked like code that never ran. `tools/test` is not affected.
  - Do: run hand checks with `YCXX_NO_DAEMON=1`, and stop your worktree's daemons
    (`pkill -f '<worktree>/build/bin/ycxx daemon'`) after rebuilding.
  - Learned on #88 step 8.
- **Eager implicit declarations can change traces for unrelated source.** Installing allocation
  functions in the shared world interned a `noexcept(true)` value before sema saw any source.
  The ordinary trace's constant IDs shifted even though its source and queries stayed the same.
  - Do: construct the same canonical entities through the existing thread-safe interners when
    sema needs them, and collect the declarations used by each unit for code generation.
  - Learned on #293.

- **An issue body rewritten through a pipe can come out empty, and `gh issue edit` takes it.**
  `gh issue view … | sed … > body.md && gh issue edit --body-file body.md` blanked #88's body once:
  the view returned nothing that time, and the `&&` only checked the redirection.
  - Do: write the new body to a file, check it is not empty (`test -s body.md`, or read it), then
    edit; or write the whole body from what you know rather than editing the old one in a pipe.
  - Learned on #88 step 9.
- **A one-OS test that asserts the other OS rejects the feature makes the parity change edit it.**
  `linux_virtual_consumer.py` checked that Darwin rejected each virtual shape. Bringing the shapes
  to Darwin (#650) meant flipping eleven of those assertions, in a test whose real checks are ELF.
  Most of the Darwin work was lifting `x86_64_linux` checks that guarded nothing target-specific:
  the lowering and the AArch64 back end already handled every shape.
  - Do: state the other host's expectation as "the same as this one" when it is, and gate on the
    target only where the code is target-specific (object format, calling convention). Before
    porting a feature, compile its fixtures for both targets and diff the results.
  - Learned on #650.
- **On macOS the configured C++ compiler is Homebrew GCC, not the platform's C++ ABI.** Apple's
  arm64 RTTI convention (private-external vague-linkage descriptors, bit 63 on the name pointer)
  and libc++ are Clang's, so a test that exchanges RTTI or `typeid` with peers needs `clang++`
  (`find_program(... clang++)`), not `${CMAKE_CXX_COMPILER}`.
  - Learned on #650.
