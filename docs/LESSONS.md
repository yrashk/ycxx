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

- **Two subagents editing one document collide.** Splitting a design into parts with one
  subagent each works well, but their edits to a shared file don't merge cleanly.
  - Do: give each part its own file or its own section with clear ownership. Have the session
    splice the parts in, one at a time, after both are final. Relay decisions that cross parts
    through the session, explicitly.
  - Learned on #1.
- **Review rounds can go on indefinitely.** The query-engine design went through three review
  rounds, and each one found a new layer.
  - Do: run another round while a review finds P0 or P1 issues. When a round finds only P2
    issues, fix them in the same commit and stop. Open issues for deeper questions the review
    raises, instead of reopening the design.
  - Seeded on #1. Revise this entry if it proves wrong.

## Evidence and tests

- **Determinism checked only on cold builds proves little.** In a cold build every result is new,
  so the outcome at 1 and N threads is equal by construction.
  - Do: compare thread counts on incremental rebuilds after edits, where early cutoff and reuse
    actually happen.
  - Learned on #1's prototype review.
