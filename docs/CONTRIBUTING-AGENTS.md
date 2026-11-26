# Contributing as an agent

ycxx is building a C++26 compiler. libycxx is its companion standard library.
The current priority is core-language support that produces runnable Linux
x86-64 programs and compiles the standard library pinned by ycxx.
[Roadmap](https://github.com/yrashk/ycxx/issues/282) ·
[Linux library coverage](https://github.com/yrashk/ycxx/issues/466)

## Pick and claim work

1. Look for `help wanted` issues. Read the issue, its recent comments, linked PRs
   and dependencies. Pick a concrete behavior to implement and a test that
   demonstrates it. On a broad issue, agree on the first slice before editing.
   [ycxx issues](https://github.com/yrashk/ycxx/labels/help%20wanted) ·
   [libycxx issues](https://github.com/yrashk/libycxx/labels/help%20wanted)
2. Use a unique, human-readable agent name and register availability, test
   platforms, models and subagent capacity in the coordination issue. Follow
   the work issue's claim procedure and post your name, branch and intended
   slice. Recheck the comments because the earlier live claim takes priority.
   Ask the coordinator to record the claim if you lack label or assignment
   permissions. The coordinator removes `help wanted` when the work is claimed.
   [Agent coordination](https://github.com/yrashk/ycxx/issues/431)
3. Read the repository rules before editing. Start with a separate worktree
   and branch, including for each implementation subagent. Agree on ownership
   before working in a shared area. Use the preservation handoff procedure for
   work covered by that handoff.
   [Agent rules](../AGENTS.md) · [Decisions](../DECISIONS.md) ·
   [Lessons](LESSONS.md) · [Status](../STATUS.md) ·
   [libycxx rules](https://github.com/yrashk/libycxx/blob/main/DECISIONS.md) ·
   [Preservation handoff](https://github.com/yrashk/ycxx/issues/377)

## Implement and verify

- Work from the project's sources, the pinned C++ draft and published
  specifications. Use GCC and upstream Clang as black-box oracles. Follow the
  repo's clean-room rules and its narrow, logged exception for inspecting
  emitted machine code. Report accidental exposure to a prohibited source on
  the issue. Have an independent specification-only author write ycxx's own
  language fixtures.
- Reproduce the failure with the current compiler and show the regression test
  fail before applying the fix. Cite the draft or ABI clause. Verify the
  positive path, the relevant diagnostic or boundary, and the behavior through
  the normal driver. Use the pinned libycxx submodule when testing ycxx.
- For query changes, compare resident results with cold results through edits.
  For ABI changes, check mixed objects from ycxx and the reference compilers.
  Record compiler differences in `docs/compatibility.toml` and exercise the
  affected compatibility modes. Follow the parity rules for target-specific
  tests.
- Run the focused tests and the repository's local gate before pushing. Use
  `tools/test --fast` for ycxx and `tools/check-all` plus the affected suite for
  libycxx. Record the platform, toolchain, base and head commits, exact commands,
  results and skips. Use repository-relative paths in shared material.
  [Build guide](BUILDING.md) · [Testing guide](TESTING.md) ·
  [libycxx build guide](https://github.com/yrashk/libycxx/blob/main/README.md)
- Follow the model and escalation rules in the repository decisions. Keep the
  work issue current with the branch, evidence, next step and any blocker.
  Link any follow-up to an issue that can receive its own implementation.

## Open a pull request

Target `main` and link the work issue. Explain the scope, show the reproducer
and summarize the before-and-after results. Use small commits and separate
review follow-ups. Keep a fork branch available for review if you lack branch
permissions in the repository.

Use a Git name and email authorized by the account holder for both the author
and committer. Check the raw identities of every introduced commit before
pushing, including merge commits. Keep the agent's display name in coordination
comments and preserve actual contributor identities when carrying over work.
[Git identity rules](../AGENTS.md#prs-and-branches)

## How maintainers review external pull requests

Maintainers review each external PR's source and check its evidence independently.
The review covers the actual candidate and its integration with current `main`.
Include the exact tested revision so maintainers can compare it with what they
are being asked to merge.

Significant changes get a second independent source review and a maintainer
replay of the highest-risk behavior. This applies to changes such as IR or query
invariants, ABI and linking, bootstrap or dependency changes, and CI or test
infrastructure. Expect scrutiny of source provenance, affected callers, failure
paths and regressions that could pass when a test exercises a different code
path. A reviewer may ask you to split a large PR so each part has a clear
correctness argument and verification.

Keep review findings and fixes visible on the PR. The merge decision requires
the applicable green gates, resolved review findings and test evidence on the
current-main integration. After merge, verify that the published commit is on
`origin/main` and update the work issue. If handing off, leave the branch,
evidence, remaining blocker and next concrete step, then release the claim.
