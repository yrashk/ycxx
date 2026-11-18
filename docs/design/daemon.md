# The resident daemon

Design for [yrashk/ycxx#83](https://github.com/yrashk/ycxx/issues/83), part of the β slice
([#84](https://github.com/yrashk/ycxx/issues/84)). Status: **accepted**, summarized in DECISIONS
§4.10.

The daemon is plumbing around the accepted [query-engine design](query-engine.md), cited as §A…
and §B…. It decides how clients reach the engine, how the file system becomes engine inputs, at
which revision a request is answered, and how the daemon lives and dies. It reopens nothing in
the engine design; where the engine as built must grow, §9 says so.

| Issue question | Where |
| --- | --- |
| Client–daemon protocol, cancellation on disconnect | §3 |
| How `ycxx cxx` finds, starts or falls back from a daemon | §7.1, §7.2 |
| File watching, inputs, `file_exists` negatives, include search paths | §5 |
| Revisions and consistency, concurrent clients, edits during a build | §4 |
| Lifecycle: idle exit, crashes, persistence handoff, memory | §7 |
| How the LSP front shares the engine; editor buffers | §6 |
| Determinism, and the β acceptance tests | §8 |
| Scope: per user or per project | §2 |

## Provenance and who decided what

Sources, per DECISIONS §2: POSIX, the Linux `inotify(7)` and `unix(7)` manual pages, Apple's
FSEvents documentation, the LSP specification, and Watchman's public documentation of its
cookie-file synchronization. No compiler's source code or design documents were consulted.

Following DECISIONS §6.5, the decision-bearing questions went to one Fable subagent, and the Opus
session that owns the issue wrote this document and the bookkeeping.

- **Fable** decided the batch admission policy and the progress quantum (§4.1–§4.2), editor
  buffers as a second view with an `editor` durability (§6.1–§6.3), input providers and the file
  table (§5.1–§5.3), freshness by cookie sync with rescan as the fallback (§5.4), the watch set
  (§5.5), one daemon per project root (§2), the per-reply counters and the test plan (§8).
- **The Opus session** proposed the plumbing (transport, framing, versioning by build hash,
  requests, the client writing outputs, discovery, fallback, crash retry, locks, idle exit), which
  Fable checked against its decisions and found no conflict. On pushback, Fable revised the root
  rule (nearest marker instead of outermost, for worktrees and home-directory repositories), and
  refined the session's rules for `build_time` (an in-place reset of an unread value, without
  which a `__TIME__` read after an idle gap would be stale) and the optimization-cache snapshot
  (its own durability) (§4.4).
- **Review.** One independent Fable check found 1 P0 (relative paths shared between requests run
  in different directories), 7 P1 and 6 P2, each with a failing scenario and a smallest fix; all
  were applied in one commit. A check of that diff alone found the 14 closed, and one new P1: a
  `pp` index without the spelled path would give two identical headers one `__FILE__`. The
  session applied that one-line fix and two P2s without a further round (DECISIONS §8.6).

## 1. Decisions in brief

1. **One daemon per project root and compiler build.** The root is the nearest ancestor with a
   `.ycxx` marker, else the nearest version-control root (a submodule does not count), else a
   per-user home scope. Configurations share one daemon, because prefixes are rooted at the
   configuration. (§2)
2. **A Unix socket in a per-user 0700 directory, named by the scope and the build hash.** Messages
   are length-prefixed and serialized by the engine's reflection serializer. Client and daemon are
   always the same binary, so the protocol has no cross-version compatibility. (§3.1)
3. **One `compile` request covers the whole `c++` command line.** The daemon answers with stdout,
   stderr, the exit status and the output bytes, and the client writes the files with the
   in-process code. A disconnected client stops its request's root. (§3.2)
4. **Batches are applied at once, behind a progress quantum.** In-flight roots stop and restart
   at the new revision inside the daemon, so the client sees one answer. The quantum grows only
   while restarts make no progress, which rules out livelock. The policy chooses the revision a
   request is answered at, and never a value. (§4)
5. **Inputs are provided on first fetch from the daemon's file table,** keyed by absolute path
   and changed only between epochs. A batch is a set of dirty paths, re-stat'ed and re-hashed; a path no input cell knows is
   dropped, which is how the build's own outputs fall out. (§5.1–§5.3)
6. **A request starts only after every change made before it was sent is applied:** a cookie file
   on the watcher's queue marks the point, with a full rescan as the fallback. (§5.4)
7. **Editor buffers are a second view of the files, in one engine.** Buffers are inputs of a new
   lowest durability, `editor`, so a keystroke leaves every disk-view cell green in O(1). Builds
   never see unsaved text; content-addressed results are shared between the views. (§6)
8. **The `ycxx cxx` path is the same code with or without a daemon,** and falls back to running
   in-process on any failure, so stdout, stderr and exit status are those of a cold compile. A
   crashed daemon is restarted and the request retried once, then run in-process. (§7.1, §7.2)
9. **Idle exit after 30 minutes, the persistent cache written at quiescence and on exit,** and
   loaded by the next daemon as red/green from a different start state (§A7.2). (§7.4, §7.5)
10. **Every reply carries the revision it was answered at and the engine's counters.** The β
    slice is proved by the `.incr` suite through the daemon, byte equality between the daemon and
    in-process over the α tests, and process-level tests of edits during a build, disconnects and
    crashes. (§8)

## 2. Scope: one daemon per project root

One daemon serves one **scope** for one ycxx binary. `YCXX_ROOT`, if set, is the root. Otherwise
one upward walk from the starting path finds the first directory that contains either:

- `.ycxx`, the project's explicit marker. A `.ycxx` file may say `root = <relative path>` to
  widen the scope (to join vendored nested repositories); it is the only way to widen one.
- a version-control marker: a `.git` directory, `.hg`, `.jj`, or a `.git` file. A `.git` file whose
  `gitdir` path has a `modules` component is a submodule (also one inside a worktree,
  `.git/worktrees/x/modules/<name>`): it does not count, and the walk continues, so a submodule
  belongs to its superproject. A worktree's `.git` file (`gitdir` under `.git/worktrees/`) counts,
  so each worktree has its own daemon.

Nothing found: the per-user home scope. `.ycxx` is found in the same walk, not searched for first
anywhere above: otherwise a `.ycxx` at a repository's top would put all its worktrees in one
daemon.

`ycxx cxx` starts the walk from the real path (symlinks resolved) of its first source file, else
its first input file, else its working directory, never the working directory first: CMake runs
compilers in a build directory that may lie outside the source tree. The LSP front starts from the
workspace folder. Both resolve the same markers, so an editor and a build of one project meet in
one daemon.

Configurations (Debug and Release, different targets) share the daemon: every prefix is rooted at
the configuration (§B1.1), so they never collide and still share leaves whose traces agree.

Rejected:
- *One daemon per user:* there is one revision (§A1.4), so an edit in one project would stop the
  builds of every other, and one process would hold several whole programs.
- *One daemon per (root, configuration):* splits the sharing the prefixes already make safe.
- *The outermost version-control root:* puts every git worktree of a repository in one daemon (an
  edit in one stops builds in the others), and one daemon for everything when the home directory
  is a repository.
- *The nearest marker without the submodule rule:* every submodule would get its own daemon, which
  shares nothing with its superproject's build.

## 3. The protocol

### 3.1 Transport

- **A Unix domain socket** in a per-user runtime directory created with mode 0700:
  `$XDG_RUNTIME_DIR/ycxx` on Linux, `$TMPDIR/ycxx-<uid>` on macOS, or `YCXX_RUNTIME_DIR`. The
  socket is `<scope-hash>-<build-hash>.sock`, where the scope hash is the hash of the root's real
  path and the build hash is the hash of the ycxx binary that keys the persistent cache (§A7.1),
  each written as 16 hex digits. `sun_path` holds 104 bytes on macOS, and `$TMPDIR` alone is about
  50: the client checks the length, and a path that does not fit is a fallback (§7.1) whose reason
  `-v` and `ycxx ctl status` report.
  The daemon checks the peer's uid (`SO_PEERCRED` on Linux, `getpeereid` on macOS) and closes a
  connection from anyone else.
- **Framing:** each message is a little-endian `u32` length, a `u16` message kind, then the body,
  serialized by the engine's reflection serializer (`ycxx.engine:serial`). The reader never trusts
  its input (§A7.3): a malformed message closes the connection.
- **Versioning by identity.** The build hash in the socket name means a client only ever talks to a
  daemon built from the same binary. The handshake, `hello{build_hash, schema_hash, uid, pid}`
  answered by `welcome{daemon_pid, root}`, guards against a stale socket file and a hash collision;
  on a mismatch the client treats the daemon as absent. Message layouts can therefore change
  freely, and the schema hash is derived by reflection from the message types, as the cache's is.

Rejected:
- *TCP on localhost:* needs authentication that a 0700 directory and peer credentials give for free.
- *One socket per user for every ycxx binary, with version negotiation:* a bootstrap build runs
  stage-1 and stage-2 compilers side by side, and they would fight over one daemon.
- *JSON:* the messages carry object files and serialized values, and the serializer exists.

### 3.2 Requests

A connection carries one session, chosen by its first request after the handshake:

| Request | Answer |
| --- | --- |
| `compile{argv, cwd, env, build_time}` | optional `progress` messages, then `done{exit_status, stdout, stderr, outputs: [(path, bytes, mode)], report}` |
| `status{}` | the daemon's state and counters (§8.1) |
| `shutdown{}` | the daemon finishes the requests it is serving, writes its cache and exits |
| `lsp{}` | the connection becomes an LSP tunnel (§6.4) |

`compile` covers every action of the `c++` command line: linking, `-c`, `-fsyntax-only` and `-E`
are arguments, parsed by the daemon with the code the in-process path uses. `env` is the closed
list of environment variables the driver reads (`SDKROOT`, `DEVELOPER_DIR`, `CPATH`,
`CPLUS_INCLUDE_PATH`, `SOURCE_DATE_EPOCH`), declared in one table that both paths read, so a
variable the driver reads cannot be missing from the request. The table has two parts:
configuration variables, interned into the root of the request's prefixes (§4.4), and rendering
variables (whether stderr is a terminal, colour and width settings), which reach only the
rendering of `done` and never a key, so a terminal build and the LSP still share their cells. `build_time` is the client's clock (§4.4).

**The request is self-contained.** The client expands `@file` response files (relative to its own
working directory) and reads `-` (standard input, as in `echo | ycxx cxx -E -x c++ -`) before
sending, and the bytes of `-` travel in the request. At admission the daemon makes every path
argument absolute against `cwd` (§5.2).

**The client writes the outputs.** The daemon answers with the bytes of every output file, and the
client writes them with the in-process path's `write_file`, with its own working directory, umask
and permissions. Object files cross the socket: a Unix socket moves gigabytes per second, and a
large link output is tens of megabytes. Drop-in behaviour stays in one place: the same code writes
`-o /dev/null`, refuses a directory and sets the executable bit, whether a daemon ran or not. A
request restarted inside the daemon (§4.1) still writes its files once.

**Diagnostics are rendered once per request,** by `tu(TU)` and the driver's merge (§A5.2), and
reach the client in `done`. Nothing a cancelled revision produced reaches a client. `progress`
messages (TUs started and finished, counters) are for status displays; `ycxx cxx` prints them only
under `-v`.

**Cancellation.** The daemon watches each connection for hang-up. A client that disconnects (a
build system's `^C` kills its compilers, which closes their sockets) has stop requested on its
request's root (§A4.1). Every cell the request completed survives (§A4.2).

## 4. Revisions and consistency

The engine has one revision, advanced only at quiescence: every root in the epoch is stopped and
joined before a batch of input changes applies (§A4.1). The daemon decides *when* a batch applies.

### 4.1 Batches apply at once, and stopped roots restart inside the daemon

When the edit strand has a pending batch, it applies it at once (subject to §4.2): it opens a new
epoch, requests stop on every root of the old one, joins, writes the inputs and opens the gate.
Each stopped request is respawned into the new epoch by the daemon. The client sees one answer.

- A restart loses only the cells in progress; every finished cell survives (§A4.2), so a restarted
  build continues where it stopped, and re-verifies what the batch could have changed.
- `make -j` throughput is unaffected by its own outputs: `.o`, `.d` and temporary files never
  become a batch (§5.3), and the objects a link reads are not file-table entries (§5.2). A
  generated header is a real change, which a cold build would also see.
- An LSP edit applies at once, and its interactive request runs ahead of the restarted background
  build (§A4.4).

Rejected:
- *Defer a batch until in-flight roots finish:* LSP latency would be that of a whole build, which
  §A4.4 rules out, and it saves no work, because a batch stops the world either way.
- *A policy by kind of change (buffer versus disk):* with one revision both stop the world. What
  differs is what turns red, and §6.3 makes a keystroke cheap there.
- *Snapshot isolation, or a second engine for builds:* rejected by §A4.1 and §6.

### 4.2 The progress quantum

The strand admits the next batch no sooner than a **quantum** after the previous one. The quantum
starts at a floor (50 ms, configurable). It doubles whenever a root is restarted without having
stored a single cell since its previous restart, and returns to the floor once every live root has
stored one.

Continuous keystrokes or a stream of generated headers therefore cannot starve a build or one long
cell (a large function's code generation): the quantum eventually exceeds the cell's duration.
With short cells the floor is the only delay.

The idle optimizer's roots (§A8.2) neither double the quantum nor hold it up: any batch stops
them, and they are respawned only when no client request is live and no batch is pending.
Otherwise one long solver leaf, restarted by each keystroke, would push the quantum to tens of
seconds and make every LSP edit and every compile wait for it. The clock is part of the policy and
never of a value: it chooses only the revision a request is answered at, and §A4.3 is untouched.

Rejected: a fixed grace period, which either livelocks a long cell or taxes every LSP edit.

### 4.3 What an answer reflects

A request becomes eligible once every change made before it was sent has been applied (§5.4).
It is then spawned into the open epoch and waits only at that epoch's gate. No request waits for
another request to finish.

A `compile` answer is the cold compile of the daemon's file table (§5.2) at the revision the request
*completed* at, and every entry of that table is at least as new as the moment the request was
sent. With no concurrent edits, that is the disk. When files change while a compile runs, a cold
compiler also reads a mixture of old and new; the daemon's answer is a consistent state no more
than one watcher latency old, and the next request sees everything. The `done` message names the
revision and how many times the request was restarted (§8.1).

### 4.4 Inputs that are not files

- **Configuration** is not a global input: the command line, the environment table and the target
  are interned into the root of the request's prefixes (§B1.1). Concurrent requests with different
  configurations never touch each other's inputs.
- **`build_time`** (§A1.1, §B9). A request whose environment sets `SOURCE_DATE_EPOCH` takes
  `__DATE__` and `__TIME__` from it, and it is already interned in the configuration root:
  `pp` reads the time through a transparent helper that answers from the configuration when it
  has the variable, and fetches the global input otherwise. A reproducible build therefore never
  sees another request's clock.

  Clock-timed requests share one global input, `build_time`, which never enters a key. When such
  a request's time, at `__TIME__`'s one-second resolution, differs from the input's value:
  - if nothing has fetched the input since it was last set, the daemon resets it in place, with
    no new revision and no quiescence, through `Db::reset_unobserved<Q>(k, v) -> bool`. An input
    cell carries an "observed since set" flag. A fetch sets the flag before it reads the value;
    a reset succeeds only if it finds the flag clear, and publishes the value and fingerprint
    under the cell's claim, so a fetch never sees a torn value. A racing fetch makes the reset
    fail, which falls to the next case. No frame and no stored cell depends on a value nobody
    read, so §A4.3 holds; `changed_at` becomes the current revision, so earlier readers stay red.
    Where the flag lives (the state word of §A1.6 has no spare bit) is the engine issue's choice.
  - otherwise the change joins the pending batch, admitted under the quantum (§4.2).

  Projects that never use `__DATE__` or `__TIME__` never pay a stop; one that does pays at most
  one per quantum while requests keep arriving. A request may be answered with a slightly later
  second, which is still a time of translation. `build_time` has the `ambient` durability (below),
  so a change walks only the cells that read it. The comparison tiers of §8.2 set
  `SOURCE_DATE_EPOCH`, so they compare exactly.

  Rejected: `build_time` in the request's root key (a repeated request of an unchanged TU would no
  longer be green in O(1), and selection cells would multiply per second); a batch per request (a
  stop every second during every build); adding the change to a batch only when the input has
  been read (a TU that uses `__TIME__` after a long idle gap would read a stale time).
- **`opt_cache_snapshot`** (§A8.3) advances only inside a batch that is applied anyway, or when no
  client request is live; the idle optimizer's own roots do not count, and are stopped and
  respawned as §4.2 says. Between advances, idle results accumulate in the optimization cache,
  invisible to the engine, so a build sees one stable snapshot. The snapshot has the durability
  `ambient`, between `editor` and `low`, which it shares with `build_time`: an advance walks only
  the cells that read one of them (the `best_available` chain, regions that use `__TIME__`), and a
  keystroke walks none of them. Rejected: advancing on every
  idle result (a stop per result), and `low` durability (every advance after idle time would make
  the next build deep-verify the whole project).

## 5. Inputs, the file table and watching

### 5.1 Inputs are provided on first fetch

Includes are discovered while preprocessing, so the daemon cannot set every `file_text`,
`file_exists` and `dir_entries` before a request reads it, as the engine requires today. The engine
gains **input providers**: `Db::set_provider<Q>(fn)`. Executing an input cell that has no value
calls the provider synchronously, stores its value with `changed_at = R`, and gives a file input a
durability chosen by its path (the toolchain and sysroot `high`, `-isystem` directories `medium`
as #17 settles, everything else `low`).

§A4.3 holds: the first observation of a cell defines it at `R`, and no frame can have seen another
value. The durability shortcut holds too, because a first value bumps no `last_changed`: nothing
depended on the cell before.

Rejected: setting every input in advance (the include graph is not known), letting queries read
the file system (§A1.1), and stopping the epoch to set each newly discovered input (hundreds of
stops in one cold build).

### 5.2 The file table

Providers read the daemon's **file table**, never the disk directly. Its keys, like the keys of
the file inputs, are **absolute paths**, lexically normalized but with symlinks kept: at admission
the daemon makes every path argument and every `-I` directory absolute against the request's
`cwd`. Two requests with the same argument vector run in different directories (recursive make:
`cd a && ycxx cxx -c -I. x.cpp`, then the same in `b`) therefore never share a `file_text` or an
`include_resolution`. The spelling a user wrote, which `__FILE__` and diagnostics show, is carried
as data in the TU's arguments and in `include_resolution`'s answer, never in a key. An entry maps
an absolute path to its real path, its stat identity (device, inode, size, modification and change times), its
content fingerprint and its text handle, or to *absent*. An entry is filled on its first read
(stat, read, hash) and changes only when a batch applies, between epochs. Every frame of an epoch
therefore sees the same table. Negative lookups along include search paths are entries too:
*absent*, with the nearest existing ancestor directory recorded for the watcher.

Only files discovered while compiling get entries. Inputs the command line names that are not
sources (objects, archives, linker scripts) are stat'ed and hashed at admission and interned by
fingerprint into the request's root, with no entry and no watch: they can be enumerated up front,
unlike headers, and otherwise every `.o` a build rewrites after its first link would become a
batch that stops the world.

The table is persisted with the cache (§7.5). It is the set of "every input file it knows about"
that §A7.2 re-hashes on a restart.

### 5.3 A batch is a set of dirty paths

Watcher events mark paths dirty; they carry no payload. Applying a batch, for each dirty path and
its parent directory: re-stat and re-hash, and `Db::set` only if the fingerprint changed (`set`
already ignores an equal value). Every dirty path with an entry is re-hashed, whether or not its
stat identity changed: a same-size rewrite within one modification-time tick leaves the stat
identity equal, and hashing costs little next to a stop. A dirty path is dropped when it has no
file entry, and its parent has neither a `dir_entries` cell nor a negative `file_exists` entry.

That one rule is how the build's own `.o`, `.d` and temporary files fall out, with no list of
build outputs. It stays exact if some query does enumerate a build directory. An editor's atomic
save (write a temporary file, rename it over the target) reads as one modification of the target.

Rejected: filtering events by the `-o` paths or by file extension, which would be wrong against a
cold compile that enumerates the directory.

### 5.4 Freshness: cookie sync, with a rescan as the fallback

Watchers deliver asynchronously, so a file saved just before `make` runs may not have reached the
daemon when the compile request arrives. When a request arrives, the daemon creates a **cookie
file** in its runtime directory, which is on the same FSEvents stream or inotify descriptor as the
watched roots, and marks the request eligible once the cookie's event is dequeued: every event
generated before it has been queued by then (Watchman documents this technique). Requests that
arrive while a cookie is in flight share the next one. A root on another volume than the runtime
directory gets its own cookies in `<root>/.ycxx-cookies/`, created with a `.gitignore` of `*` so it
stays out of version control; events inside it never become batches. (`.ycxx` is §2's marker and
is never written by the daemon.)

The daemon falls back to a **full rescan** (stat every table entry and re-hash the changed ones, the
§A7.2 procedure) on queue overflow (`IN_Q_OVERFLOW`; FSEvents' `MustScanSubDirs` or
`UserDropped`), on a cookie timeout, on watcher failure, and always under `YCXX_NO_WATCH=1`.

Rejected: stat-validating the table on every request, which costs tens of milliseconds for a whole
program per request; it is kept only as the fallback.

### 5.5 What is watched

- **macOS:** one FSEvents stream, recursive, over the scope root, the runtime directory, the
  toolchain and sysroot directories, and every directory outside the root that holds a table
  entry (a build directory with generated headers).
- **Linux:** one inotify descriptor with one watch per directory that holds a table entry,
  positive or negative, plus the nearest existing ancestor of a missing directory; the watch moves
  down when the directory appears. Files created in the new directory before its watch was added
  produced no event (`mkdir inc/sub && cp x.h inc/sub/`, or a checkout), so adding a watch to a
  directory that appeared marks every table entry beneath it dirty, in the same batch. At `max_user_watches`, unwatched directories are stat-checked per
  request instead, and `status` says so.
- **Symlinks:** watches are on real directories. A reverse index maps a real path to the spelled
  absolute paths that reach it, and a change to a link's directory re-resolves its entries. Keys
  stay the absolute path with its symlinks (§5.2).

## 6. Editor buffers and the LSP front

### 6.1 Two views of the files, one engine

Unsaved buffers must drive LSP answers, and must not change what `ycxx cxx` produces. The engine
holds two **views**:

- the **disk view**: the inputs `file_text`, `file_exists` and `dir_entries`, as today;
- the **editor view**: new inputs `buffer(path) -> optional<text>` (absent when the file is not
  open) and `buffers_in(dir) -> sorted paths`.

Files are read only through transparent helpers `text(view, path)`, `exists(view, path)` and
`entries(view, dir)`, and only from the question cells rooted at a view: `include_resolution` and
the TU's own source. `pp`'s index is the content identity and the spelled path (as `__FILE__`
prints it) from `include_resolution`'s answer, so no
trace-recorded leaf ever records a `buffer` or `buffers_in` dependency; if one did, a disk-view
cell reusing that leaf would inherit the `editor` durability, and every keystroke would turn the
build's shared leaves red. Debug builds assert that no such dependency appears under a disk-view
prefix.

In the editor view the helpers fetch `buffer(path)` and fall back to the disk input when it is
absent; `exists` is "a buffer is open or the disk has it"; `entries` is the
disk's entries plus `buffers_in`. Only the root of the prefixes, the configuration, gains a `view`
field. Every prefix is rooted there, so `include_resolution(…, config)` and every contextual key
inherit the view without a new dimension on each kind.

A buffer for a file not yet saved exists in the editor view, so include resolution finds it as it
will after the save; the disk view is untouched. Documents without a path (untitled buffers) map
to no file.

Rejected:
- *One `file_text` where the buffer wins:* builds would compile unsaved text.
- *A separate engine for the editor:* it would duplicate the memo table for every header the open
  files include, lose the cross-TU results that references and `odr_report` need, and double the
  persistent cache.
- *The set of open buffers as one input:* opening any file would turn every editor-view cell red;
  `buffer(path)` turns one path red.

### 6.2 What the views share

Everything content-addressed is shared: token regions, trees, interned types, the `cheap` indexes,
and trie leaves (header analyses, instantiations, constant evaluations) wherever their traces
verify (§A1.9, §B1.1). Not shared: prefix nodes, the selection and question cells at editor-view
prefixes, and `tu(TU)` of the editor view. The duplicated part is small; the work is in the leaves.

### 6.3 The `editor` durability

`buffer` and `buffers_in` get a new lowest durability, `editor`. The order becomes `editor <
ambient < low < medium < high` (`ambient` is §4.4's). A keystroke bumps only
`last_changed[editor]`, so every disk-view cell stays green by the O(1) shortcut (§A1.4), and a
build restarted by a keystroke pays for the stop and nothing else. A save sets `file_text`, which
is `low`, as any other file change.

Rejected: reusing `low` for buffers, which would make every keystroke deep-verify every project
cell of every TU being built.

### 6.4 The LSP front

`ycxx lsp` is started by the editor. It finds the daemon for its workspace folder (§2, §7.1),
sends `lsp{}`, and from then on copies bytes between its stdio and the socket (the daemon's
side in `lsp` frames, whose last carries LSP's exit status, #99). The LSP server
itself (JSON-RPC, documents, position mapping per ast.md §8.5) runs in the daemon, one session per
connection:

- `didOpen` and `didChange` set `buffer(path)`; `didClose` clears it. Each is a batch (§4).
- Requests are `interactive` roots (§A3.5). `$/cancelRequest` and a closed connection stop them.
- Diagnostics are published per file from the editor view's `tu(TU)`, debounced after edits.
- A file's TU and command line come from the `compile` requests the daemon has served, then from
  `compile_commands.json` at the root. Open documents are eviction roots (§A6.2).

Rejected: an LSP server in the client process speaking a second protocol to the daemon. Every LSP
request needs the engine, so the client would only translate, and a tunnel keeps one protocol.

## 7. Lifecycle

### 7.1 Finding and starting the daemon

`ycxx cxx`, `ycxx lsp` and `ycxx ctl` find the daemon this way:

1. Compute the scope (§2) and the socket path, and connect.
2. On `ENOENT` or `ECONNREFUSED`, take an exclusive `flock` on `<socket>.start` and connect again
   (another client may have started it meanwhile).
3. Still absent: spawn `ycxx daemon --root <root>` detached (`setsid`, stdio to the daemon's log
   in the runtime directory, every other inherited descriptor closed, so a build tool's pipes and
   make's jobserver are not held open for the daemon's life), passing the write end of a pipe. The daemon takes its lifetime lock
   (§7.3), binds, listens, writes one byte to the pipe and closes it. The client waits for that
   byte (end-of-file means the daemon failed to start), releases the start lock and connects.
4. **Fallback:** if any step fails, if `YCXX_NO_DAEMON=1` is set, or under `ycxx cxx --no-daemon`,
   the client runs the request in-process. It prints nothing about this unless `-v` is given, so
   stdout, stderr and the exit status are those of a cold compile either way.

The in-process path is not a second implementation. The binary is the same (DECISIONS §4.7): it
constructs the same engine, runs the same request handler and gets the same `done`, without a
socket and without a watcher (its file table is filled on first read and never changes).

### 7.2 Crashes

A daemon that dies mid-request shows up as end-of-file without `done`. The client restarts the
daemon (§7.1) and retries the request once; a second failure runs it in-process. A genuine internal
compiler error (a contract violation, a query cycle) then fails in-process exactly as a cold
compile fails, with its message and status. The crashed daemon's log keeps the request and the
stack. A retried request may land on an empty or freshly loaded table, which changes nothing but
its cost.

### 7.3 One daemon per scope

The daemon holds an exclusive `flock` on `<scope-hash>-<build-hash>.lock` for its whole life. A
second daemon for the same scope waits for it, for a bounded time (10 s), and exits if it does not
get it. The wait covers a daemon that is exiting and still writing its cache: otherwise every
compile started in that window would fall back to in-process. The kernel releases the lock however
the holder dies, so a crash never leaves a stale lock; the next daemon unlinks a stale socket file
after it takes the lock.

### 7.4 Idle exit and shutdown

The daemon exits after a configurable idle time (default 30 minutes, `--idle-exit`) with no
connection. Background optimization (§A8.2) does not count as activity; its results are already in
the optimization cache. Before exiting it writes its persistent cache at quiescence and removes its
socket. `ycxx ctl shutdown` and `SIGTERM` do the same once the requests in flight finish.

### 7.5 Persistence handoff

The cache lives in `$XDG_CACHE_HOME/ycxx/<build-hash>/<scope-hash>/` (`~/Library/Caches/ycxx/…` on
macOS), written only by the holder of the scope's lock. It includes the file table (§5.2). The next
daemon loads it as §A7.2 describes: records start at `verified_at = 0`, every file-table entry is
re-hashed in parallel, and the first request runs ordinary red/green from that state. A crash loses
at most what was not yet written: segments are written at quiescence, and a torn tail is truncated
(§A7.3).

The optimization cache (§A8) is per user, not per scope, written as per-daemon append-only segments
so daemons never collide (#30). Reusing high-durability records across scopes, merged by
fingerprint as §A7.1 permits, is a later improvement (#17).

### 7.6 Memory

The daemon applies §A6.2's eviction at quiescence with the configured budget (default and policy:
#17). Its roots are the open LSP documents and the last request of each command line served in the
last `K` revisions. `status` reports the memory held by the interners and the memo table.

## 8. Determinism and the β acceptance tests

A daemon build is bit-identical to a cold in-process build of the same file state. That follows
from §A4.3 and §A5, and from §4: the admission policy chooses only the revision. The tests below
check it.

### 8.1 What the daemon reports

Every `done` carries the revision it was answered at, the number of restarts, the inputs changed in
the epochs it ran through, and the engine's `Stats` (executed, backdated, validated, durable, hits)
counted **per root**: each frame knows its root, so a concurrent client or the idle optimizer
does not show up in another request's counts. Debug builds break them down per query kind. `ycxx ctl status` reports
the same totals, plus the watcher's state, the cache size, memory and the longest join latency.

### 8.2 The tests that prove the slice

- **`tests/incremental` through the daemon.** The hook in `tests/driver/incremental.py` starts a
  daemon on a private `YCXX_RUNTIME_DIR` and cache directory, writes state 0, runs the action
  through `ycxx cxx`, then writes each edit's files and runs it again. File writes and `ycxx cxx`
  are the whole protocol. The existing comparison with a cold run stands, and two checks are added:
  an identical repeated request reports `executed == 0`, and an edited state reports no more
  executions than the cold run of that state (a test may mark that it expects strictly fewer). ycxx then lists the `incremental` feature.
- **Daemon versus in-process.** A driver tier runs `tests/own/slice` and `tests/own/smoke` twice,
  through a daemon and under `YCXX_NO_DAEMON=1`, and compares stdout, stderr, exit status and the
  output bytes, at 1 thread and at `P`. It includes one case that compiles the same argument
  vector from two working directories with different local headers.
- **Process-level tests, a new suite `tests/daemon/`** (outside `tests/own`, which holds the
  spec-only language tests of DECISIONS §5.3):
  - an edit during a build: a slow TU and an edit that changes its output; the answer equals a cold
    compile of the final state, with `restarts >= 1`;
  - a client killed mid-compile: its root is stopped (in `status`), and the next request is right;
  - the daemon killed mid-request: the client retries and the output equals a cold compile;
  - shutdown and restart: the cache loads, `executed` is below a cold run's, and
    `durable + validated > 0`.
- **Unit tests** (`tests/unit/daemon`): framing round trips and malformed messages, the admission
  state machine with simulated time (the quantum doubles and resets), dirty-path application
  against a fake file table, and root discovery (worktrees, submodules, home).
- **The memo-table comparison** (`ensure_green` with zero executions) and chaos seeds belong to
  the harness, #19, through a `ycxx ctl verify-table` request, with the daemon honouring
  `YCXX_THREADS` and the chaos seed.

## 9. What other parts must provide

- **Engine** (§A1, as built in `src/engine`): input providers (`Db::set_provider<Q>`), called
  where a missing input is a contract violation today; per-cell durability for input cells;
  `durability_levels = 5`, `editor < ambient < low < medium < high`; `Db::set` returning whether
  the value changed; `Db::reset_unobserved` with an "observed since set" flag on input cells
  (§4.4); `Stats` counted per root, and per kind in debug builds.
- **Scheduler** (#15): the epoch strand of §A4.1 with respawn of stopped requests into the new
  epoch, and the admission quantum's hook (a root's "stored a cell since its last restart").
- **Preprocessor** (§B9): `buffer` and `buffers_in`, the transparent `text`, `exists` and `entries`
  helpers called only from view-rooted question cells, `pp` indexed by the content identity that
  `include_resolution` answers together with the spelled path (two identical headers reached by
  different spellings are two leaves, as `__FILE__` requires), `view` in the configuration root, and the
  time helper that prefers the configuration's `SOURCE_DATE_EPOCH` over `build_time`.
- **Driver** (`client::cxx`, #63): path arguments made absolute against `cwd` at admission,
  response files and `-` read by the client, and non-source inputs interned into the request's
  root.
- **Persistence** (#17): the file table in the cache, and the cache directory per scope.
- **Harness** (#19): `ycxx ctl verify-table`, and `YCXX_THREADS` and the chaos seed in the daemon.

## 10. Follow-up issues

Implementation, one issue per part:

| Issue | Area | Content |
| --- | --- | --- |
| [#95](https://github.com/yrashk/ycxx/issues/95) | `area:query-engine` | Input providers, `editor` and `ambient` durabilities, `Db::set` reporting a change, `reset_unobserved`, per-root `Stats` (§9) |
| [#96](https://github.com/yrashk/ycxx/issues/96) | `area:daemon`, `area:driver` | Scope, transport, requests, discovery, fallback, crashes, locks, idle exit, handoff (§2, §3, §7) |
| [#97](https://github.com/yrashk/ycxx/issues/97) | `area:daemon` | File table, dirty-path batches, cookie sync, watchers, admission and the quantum (§4, §5) |
| [#98](https://github.com/yrashk/ycxx/issues/98) | `area:tests`, `area:daemon` | The β tests (§8.2) |
| [#99](https://github.com/yrashk/ycxx/issues/99) | `area:lsp`, `area:pp` | The editor view and `ycxx lsp` (§6) |

Routed to existing issues: the epoch strand's respawn and the quantum hook (#15); the file table
in the cache, the per-scope cache directory, and reusing high-durability records across scopes
(#17); `ycxx ctl verify-table` (#19); the optimization cache per user and its snapshot's
durability (#30); the driver's admission of paths and non-source inputs (#63).
