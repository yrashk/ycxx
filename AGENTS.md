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

## PRs and branches

Work that never reaches `main` is lost. Twice, finished and reviewed work landed on a dead branch
(LESSONS: #198, #233).
- Every PR targets `main`. Stack at most one step on an unmerged PR. When its base merges,
  retarget yours to `main` (`gh pr edit N --base main`) before anything else.
- Agent display names identify sessions in coordination comments. For commits agents create,
  including merge and rebase commits, use the name and email the intended GitHub account holder
  has authorized. Use the exact address associated with that account. Never construct a
  `@users.noreply.github.com` address from an agent display name. GitHub uses commit email
  addresses to assign attribution.
- Before committing, check `git var GIT_AUTHOR_IDENT` and `git var GIT_COMMITTER_IDENT`. Before
  pushing or merging, inspect the raw author and committer of every new agent-created commit,
  including commits brought in by a merge. For a checked-out branch, run
  `git log origin/main..HEAD --format='%h %an <%ae> | %cn <%ce>'`. Correct any mismatch on the
  unmerged branch before proceeding. Preserve the identities of existing human contributions.
- Before pushing to a PR's branch, check that the PR is still open (`gh pr view N --json state`).
  Once it has merged, anything new, even a follow-up to the same issue, goes on a fresh branch
  from `main` with a new PR.
- After your PR merges, confirm its commits are on `main`
  (`git merge-base --is-ancestor <sha> origin/main`) before you report it done.
- A PR is merged only once it has passed tests merged with the current `main`. That means CI ran
  after the last merge to `main`, or the merger ran `tools/test --fast` on the PR merged with
  `main`. A clean textual merge proves nothing: #250 merged cleanly with #243 and broke its
  concepts test (LESSONS).

## While working

- For changes to accepted syntax, semantics, preprocessing values or the ABI, check the pinned
  draft or published ABI and run the relevant GCC and upstream Clang black-box probes. When the
  answers differ, update [docs/compatibility.toml](docs/compatibility.toml) with the reference,
  observed behavior, support status for each mode, and regression coverage. Apply oracle-specific
  choices through the translation unit's `-fcompat=spec|gcc|clang` setting. Keep the draft
  expectation in `spec` mode and check every affected mode, including both cold and resident
  compilation when the choice changes a cached result.
  Record confirmed behavior that requires an unsupported language feature and name the prerequisite.
- Keep the extensions in [docs/EXTENSIONS.md](docs/EXTENSIONS.md) available in every mode.
  System and libycxx headers rely on these extensions and their feature probes. Record any
  mode-dependent behavior of an extension in the compatibility table.
- Write C++ the way fully implemented C++26 would have it (DECISIONS §3.2): concepts,
  modules, reflection instead of generators and boilerplate, contracts, coroutines and
  `std::execution`, `constexpr`. A bootstrap-compiler limitation gets the smallest workaround,
  marked `// BOOTSTRAP: <feature>: <reason>` and listed in STATUS.
- Run `tools/test --fast` before pushing; CI confirms, it does not debug (DECISIONS §9).
- To run tests without building, name the stage: `tools/test core <path>`, or
  `tools/test --cxx clang affected <path>` for an oracle. A path alone also runs `tools/build`.
  Never run two builds in one build directory. They broke modules and cost 15–37 minutes four
  times (LESSONS, #659, #218, #259, #309).
- Triage timeouts by checking `uptime` and rerunning the timed-out CTests with
  `ctest --test-dir build -R '<names>' -j 2`. Use `tools/test failed affected core` to rerun the
  driver stages without rebuilding. A passing isolated rerun can point to contention and can
  miss a concurrency bug. The required gate still has to pass for the exact merge candidate,
  either locally under suitable load or in CI. Timeouts cost about an hour on #330 under load.
  On macOS, 10 s program timeouts also fail gates at low load (#1123), so a low load does not make
  one a real failure, nor a high load make it harmless (LESSONS).
- macOS and Linux are meant to do the same things. A test or CTest you limit to one OS carries
  `PARITY:` with the issue that brings it to the other, or `by design:` with the host fact it is
  about. `tools/check-os-gates` enforces this for `REQUIRES:`, `ycxx.xfail` and `ycxx.txt` (an
  issue only) and `tests/**/CMakeLists.txt`; a gate inside a test script or CI job needs the
  comment by hand. A capability you add on one OS only gets a row in the macOS ↔ Linux parity
  issue (#659) or the ISA parity issue (#639).
- If you burn time (retries, review rounds that don't converge, a dead end, a brief that came back
  off-target) and then find the way out, record it in `docs/LESSONS.md`, or as a rule in
  DECISIONS or here (DECISIONS §8).
- **Don't get lost in bureaucracy loops** (DECISIONS §8.6). Extra review rounds, evidence nobody
  reads, repeated relays, waiting for approvals you don't need, and documents that grow faster
  than their decisions all waste time. Before each such step, ask what bug, decision or
  unblocked work it produces. If the answer is none, skip it and say so in one line.
- Keep the issue body current: done, next, blocked. Comment on decisions and surprises.
- Small commits, reviewed P0–P2 (DECISIONS §5). Cite draft clauses by stable name (`[temp.deduct]`).
- When a change makes the item splitter meet a new token form (a specifier that takes
  parentheses, or a form a gate used to reject), grep `items.cppm` for the token and test the form
  at namespace scope and as a member followed by each thing that can come next. Its guesses fail
  silently: #211 lost a typedef and #311 ran the next member's body (LESSONS).
- A handle's `{}` is index 0, a real type, entity or list, never "none". Say "none" with the
  kind's `none` value (`EntityId::none`, `NodeId::none`), a flag or `std::optional`, and give
  a new handle field `= X::none`. Four bugs came from `T{}` (LESSONS, #25 and #88).
- A design question beyond your issue goes to a new issue labelled `status:needs-decision`,
  and `model:fable` if it is hard. Don't decide it silently in code.
- **Escalate design sparingly** (DECISIONS §6.5). Use Opus at medium effort for routine
  implementation, code review and tests, and high effort for intricate work. Use Codex
  Sol at high effort for implementation, code review and tests. Sol medium is for
  mechanical work.
  - Opus escalates to Fable. Sol escalates to Astra. Give the design model a self-contained
    brief for a design decision, hard specification question, independent design check, or
    diagnosis after two serious failed implementation attempts.
  - Include the question, prior attempts when relevant, stable draft clauses, applicable
    decisions and the rule never to read compiler source. Review the answer and follow
    up on disagreements. Attribute conclusions you use. Keep writing, implementation,
    verification and issue bookkeeping in the owning session.

## File references in shared artifacts

Use repository-relative paths in issues, PR descriptions and comments, Markdown files,
and commit messages. Add a repository name or a GitHub permalink when a reference crosses
repositories. Replace machine-local absolute paths in pasted commands, logs and diagnostics
with repository-relative paths or placeholders before publishing.

## When you stop

- Every handover or closing comment answers: *what would have made this faster?* Add a lesson
  if the answer is not trivial.
- **No follow-up is lost** (DECISIONS §6.7). Every follow-up you name (a deferred finding, a
  "later", a known limitation, a stand-in to replace) goes to an existing issue (comment there)
  or a new one (`area:*`, `kind:*`, milestone, failing example, clauses), linked both ways. A
  handover, closing comment or PR body is incomplete until each follow-up in it has its issue
  link; PRs without them are not merged.
- Finished: update `STATUS.md` if what is true of the code changed, and the README's Status
  section if what a user can do changed (DECISIONS §6.8). Then close the issue with a summary and
  remove `status:claimed`.
- Not finished: post a handover comment (state, branch, what you learned, the next concrete step)
  and remove `status:claimed`.
