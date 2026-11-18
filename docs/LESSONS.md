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

## Evidence and tests

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
