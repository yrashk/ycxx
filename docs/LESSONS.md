# Lessons: common solutions

Recipes for situations agents keep running into (DECISIONS §8). Each entry gives the symptom,
what to do, and where it was learned. Read this file before starting work. Add an entry when you
recover from a time sink, fix an entry that turned out wrong, and delete one that no longer
applies. A lesson that becomes a rule moves to DECISIONS or AGENTS.md and leaves a pointer here.

## Orchestration and sessions

- **A session cannot run on a model above its parent's.** A session asked to run on Fable
  silently started on Opus.
  - Do: run sessions on Opus and reach Fable through Agent subagents with `model: "fable"`.
  - Now a rule: DECISIONS §6.5.
  - Learned on #1, 2026-11-17.
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

## Bureaucracy loops

- **A design document became a loop.** The query-engine design (#1) grew to 31,500 words over
  seven rounds of pushback and three reviews, plus repeated relays between two subagents. Each
  round's fixes added text, and the next round found new problems in that text.
  - Do: write decisions, reasons and rejected alternatives, nothing more.
  - Do: settle mechanism questions in the spike, not in prose.
  - Do: stop reviewing when a round finds no P0 or P1 issues (DECISIONS §8.6).
  - Learned on #1.

## Toolchain

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

- **GCC 16.2 module bugs that fail silently or late.** A defaulted friend `operator==` in an
  exported class segfaults the importer; `pre` on a function template is silently dropped when
  the template is instantiated through an import. Both are in STATUS's workaround list.
  - Do: test that a contract fires (a violation-handler test, as `unit.engine_contract`), not
    only that it compiles. Reduce a module ICE to two files with `g++-16 -fmodules` first.
  - Learned on #14.

- **An executable killed at exec with status 137 says nothing about why.** A linked Mach-O whose
  `ncmds` was one too high passed `codesign -v`, then the kernel killed it before `main`.
  - Do: run `otool -l` on the output first. It reports "malformed load command" and where.
    `dyld_info -fixups -exports -imports` checks chained fixups and the exports trie the same way.
  - Learned on #42.
- **libycxx deprecates `std::filesystem::path::string()`** (C++26 [depr.fs.path.obs]), and
  `-Werror` turns it into a build failure.
  - Do: use `native_encoded_string()` for file names passed on, `display_string()` for messages.
  - Learned on #42.

## Evidence and tests

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

- **Test a selection's invariant end to end, not just its helper.** The test driver's shard
  helper was unit-tested and correct, but main.py fed each stage a list that depended on earlier
  shards' picks, so shards skipped some tests and doubled others: CI could go green without
  running them. A review found it; a self-test running `tools/test --list --shard i/N` for every
  i and checking the union equals the unsharded list now guards it.
  - Do: for partitioning, ordering or filtering code, check the invariant (every item once,
    nothing lost) through the real entry point, with state that makes the stages interact.
  - Learned on #5.

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
