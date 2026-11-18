# Working on ycxx

Read [DECISIONS.md](DECISIONS.md) before changing anything. Read [STATUS.md](STATUS.md) for what
exists. Work is coordinated through [GitHub issues](https://github.com/yrashk/ycxx/issues).

## Never read

The source code, headers or generated code of other compilers, assemblers or linkers:
Clang/LLVM, GCC/binutils, MSVC, EDG, lld, mold, ld64, Zig. This includes snippets found in web
search results, blog posts that quote them, and Stack Overflow answers that paste them. If you
land on such content, stop reading it, and say so in your issue comment. Running those compilers
as black-box oracles is allowed (DECISIONS §2.3).

## Before you start

1. Find or open the issue for your work: `gh issue list -R yrashk/ycxx`.
2. Claim it (DECISIONS §6.2):
   - `gh issue edit N -R yrashk/ycxx --add-label status:claimed --add-assignee yrashk`
   - `gh issue comment N -R yrashk/ycxx -b "Claimed by <session name> (<branch>)"`
   - Re-read the issue's comments. If an earlier claim exists, back off.
3. Work in your own worktree and branch. Stay inside your issue's `area:*` labels.

## While working

- Keep the issue body current: done, next, blocked. Comment on decisions and surprises.
- Small commits, reviewed P0–P2 (DECISIONS §5). Cite draft clauses by stable name (`[temp.deduct]`).
- A design question beyond your issue goes to a new issue labelled `status:needs-decision`,
  and `model:fable` if it is hard. Don't decide it silently in code.

## When you stop

- Finished: update `STATUS.md` if what is true of the code changed, close the issue with a summary,
  and remove `status:claimed`.
- Not finished: post a handover comment (state, branch, what you learned, the next concrete step)
  and remove `status:claimed`.
