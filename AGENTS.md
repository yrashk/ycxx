# Working on ycxx

Read [DECISIONS.md](DECISIONS.md) and [docs/LESSONS.md](docs/LESSONS.md) before changing
anything. Read [STATUS.md](STATUS.md) for what exists. Work is coordinated through
[GitHub issues](https://github.com/yrashk/ycxx/issues).

## Never read

The source code, headers or build-generated sources of other compilers, assemblers or linkers:
Clang/LLVM, GCC/binutils, MSVC, EDG, lld, mold, ld64, Zig. This includes snippets found in web
search results, blog posts that quote them, and Stack Overflow answers that paste them. If you
land on such content, stop reading it, and say so in your issue comment. Running those compilers
as black-box oracles is allowed (DECISIONS §2.3).

Reading the machine code Clang or GCC *emit* is allowed only to diagnose a measured performance
gap on a specific benchmark function. Log each reading on your issue, and restate what you learn
in our own terms. Never copy sequences or commit their listings (DECISIONS §2.3).

## Before you start

1. Find or open the issue for your work: `gh issue list -R yrashk/ycxx`.
2. Claim it (DECISIONS §6.2):
   - `gh issue edit N -R yrashk/ycxx --add-label status:claimed --add-assignee yrashk`
   - `gh issue comment N -R yrashk/ycxx -b "Claimed by <session name> (<branch>)"`
   - Re-read the issue's comments. If an earlier claim exists, back off.
3. Work in your own worktree and branch. Stay inside your issue's `area:*` labels.

## While working

- Write C++ the way fully implemented C++26 would have it (DECISIONS §3.2): concepts,
  modules, reflection instead of generators and boilerplate, contracts, coroutines and
  `std::execution`, `constexpr`. A bootstrap-compiler limitation gets the smallest workaround,
  marked `// BOOTSTRAP: <feature>: <reason>` and listed in STATUS.
- Run `tools/test --fast` before pushing; CI confirms, it does not debug (DECISIONS §9).
- If you burn time (retries, review rounds that don't converge, a dead end, a brief that came back
  off-target) and then find the way out, record it in `docs/LESSONS.md`, or as a rule in
  DECISIONS or here (DECISIONS §8).
- **Don't get lost in bureaucracy loops** (DECISIONS §8.6). Extra review rounds, evidence nobody
  reads, repeated relays, waiting for approvals you don't need, and documents that grow faster
  than their decisions all waste time. Before each such step, ask what bug, decision or
  unblocked work it produces. If the answer is none, skip it and say so in one line.
- Keep the issue body current: done, next, blocked. Comment on decisions and surprises.
- Small commits, reviewed P0–P2 (DECISIONS §5). Cite draft clauses by stable name (`[temp.deduct]`).
- A design question beyond your issue goes to a new issue labelled `status:needs-decision`,
  and `model:fable` if it is hard. Don't decide it silently in code.
- **Fable, sparingly, for design** (DECISIONS §6.5). You are most likely running on Opus.
  - Use Fable (the Agent tool with `model: "fable"`) for design decisions, the hardest
    specification questions, and independent verification of a design.
  - Never use it to write code, review code, write tests, draft prose or relay messages.
  - Escalate a coding problem to Fable only after two serious Opus attempts have failed. Say
    what was tried; Fable diagnoses, and you write the code.
  - Give the subagent a self-contained brief: the question, the clauses by stable name, the
    relevant DECISIONS sections, and the rule never to read compiler source.
  - Review its conclusions before using them, and push back with a follow-up when something is
    unclear or seems wrong.
  - Say in the document or comment which conclusions came from Fable.
  - You keep the claim, the writing, the code and the issue updates.

## When you stop

- Every handover or closing comment answers: *what would have made this faster?* Add a lesson
  if the answer is not trivial.
- **No follow-up is lost** (DECISIONS §6.7). Every follow-up you name (a deferred finding, a
  "later", a known limitation, a stand-in to replace) goes to an existing issue (comment there)
  or a new one (`area:*`, `kind:*`, milestone, failing example, clauses), linked both ways. A
  handover, closing comment or PR body is incomplete until each follow-up in it has its issue
  link; PRs without them are not merged.
- Finished: update `STATUS.md` if what is true of the code changed, close the issue with a summary,
  and remove `status:claimed`.
- Not finished: post a handover comment (state, branch, what you learned, the next concrete step)
  and remove `status:claimed`.
