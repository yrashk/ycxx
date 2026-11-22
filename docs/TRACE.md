# The engine trace's JSON Lines

`ycxx cxx --trace-engine=json` writes the trace on stderr, and `ycxx ctl trace --json` writes it on
stdout (#245). Every line is one JSON object. Its keys say what it is. Fields may be added; tools
should ignore keys they do not know.

Times, workers and memory are measurements of one run on one host. They differ between runs and
never enter a value, a fingerprint or a comparison of builds (#274).

## Events

One object per cell a request settled, in the collector's batches: whole subtrees in pre-order.

| key | type | meaning |
| --- | --- | --- |
| `id` | integer | the event, unique in its engine; given in start order |
| `parent` | integer | the event of the frame that asked for the cell; 0 for a request's own cell |
| `depth` | integer | in the request's tree |
| `kind`, `key` | string | the cell: its query kind and its key as text (`Tu`, `a.cpp`) |
| `outcome` | string | `executed`, `cutoff`, `verified`, `reused`, `durable`, `input`, `recomputed`, `waited` or `failed` |
| `start_ns`, `duration_ns` | integer | since the trace began; 0 for an instant |
| `worker`, `end_worker` | integer | the pool worker it started and ended on; -1 the requesting thread |
| `request` | string | with `ctl trace`: which request |
| `value` | string | the kind's rendering of the value, when shown |
| `errors`, `error_here` | string, bool | the errors the value holds; whether they start at this cell |
| `error_at` | integer | the event under this one where its errors start |
| `repeat` | integer | identical instants in a row under one parent, given once |
| `alloc_bytes`, `allocs` | integer | bytes the cell's frame allocated while it ran, and how many allocations: its query and the engine's work for it, not what it demanded; 0 for an instant; for `input`, what its provider allocated |
| `peak_bytes` | integer | the high-water mark of the frame's own net bytes (allocated minus freed) during the run |
| `retained_bytes` | integer | what the cell retains: the net bytes its runs left alive (its value, dependency lists, and what it interned first), or an input's size; may be below zero when a frame freed what another allocated |
| `incl_alloc_bytes` | integer | `alloc_bytes` summed over the subtree, this event included |
| `incl_retained_bytes` | integer | `retained_bytes` summed over the subtree, counted once per cell where it settles (`executed`, `cutoff`, `verified`, `durable`, `input`), never below zero |
| `span` | object | where in the source the cell is (#638), when its kind can say: `file`, and `line`, `column` to `end_line`, `end_column`, where its last token starts (1-based, columns in bytes); `line` 0 means the whole file |

Memory that one cell's run allocated and others use (an interned tree, a header's region) counts
for that cell only. Memory no frame allocated (the engine's tables, the session's state) counts
for no cell.

## Source spans

A cell that corresponds to source says where it is (#638). Its kind's `trace_span` hook
reads values the build already holds (`Db::peek`, with no dependency and no execution) and runs only
for a tracer, so an untraced build pays nothing.

| cells | span |
| --- | --- |
| a file's: `SourceOf`, `FileText`, `Pp`, `RegionItems` | the whole file |
| a unit's: `TuFront`, `TuUnit`, `UnitShapes`, `Tu`, `TuSymbolAddresses` | the unit's main file |
| a function's: `FunctionSourceQ`, `FunctionGen`, `FunctionIr`, `FunctionDebug`, `Lower`, `UnitQ`, `Codegen` | its definition, from its first token to where its last starts (a token from a macro is where the macro is used); an instantiation's is its pattern's, since its body is the pattern's tokens analysed again |

These cells have none, and why:
- `Optimized`: it is keyed by the fingerprint of an optimization unit, which any number of
  functions may share.
- `Build`, `Link`, `ProgramIn`, `OptLevel` and `CostModelIn`: they belong to a program or the
  configuration, not to source.
- `LayoutConstant`: a constant's layout has no place in the source.
- `FileContent`: an include probe of a path, which may not exist.
- `DiagsOf`: it reads another cell.

Class and variable instantiations, and per-item analysis, have no cells of their own yet (#16).

A cell whose values were not computed in this request at this revision (another request owns them)
gets no span rather than a stale one.

Lines are counted in the text preprocessing read: in the editor view, an open buffer's.

The JSON's self time is computed per batch: a late child (one a failed frame left running,
handed on after its parent's batch) is not subtracted from its parent's self time there, as it
is in the browser's tree.

The filter's words are split at spaces outside double quotes: `at:"/my dir/a.cpp"`. The browser's filter takes `at:a.cpp:120` or `at:a.cpp:120:5` (the cells whose span touches line
120 of a file whose path ends in `/a.cpp`), or `at:a.cpp` (any cell in that file).

## The summary

One per build, last: `{"summary":{…}}`.

| key | type | meaning |
| --- | --- | --- |
| `executed` … `failed` | integer | events per outcome |
| `events`, `total_ns` | integer | events, and the build's time |
| `memory.alloc_bytes`, `memory.allocs` | integer | the events' allocations, summed |
| `memory.retained_bytes` | integer | retained bytes, summed as `incl_retained_bytes` sums them |
| `memory.process_current_bytes` | integer | the process's bytes allocated when the build ended (the daemon's, through a daemon) |
| `memory.process_peak_bytes` | integer | their high-water mark while the build ran, other builds running at the same time included; exact within 64 KiB per thread |
| `kinds` | array | per query kind with memory: `kind`, `executions`, `alloc_bytes`, `allocs`, `peak_bytes` (its highest frame's), `retained_bytes`; most allocated first |
| `histogram.by_kind`, `histogram.by_file` | array | the browser's histogram groups (#637), by query kind (`kind`) and by source file (`file`; `(no source)` for cells without a span), most self time first: `cells`, then one field per metric (below) |

Each histogram group sums its cells' metrics; `peak_bytes` is the highest of them (the largest
single frame's, not a subtree's). `cells` and `hits` count a folded instant (`repeat`) as often
as it was read.

| field | metric |
| --- | --- |
| `self_ns` | self time: a cell's span less the union of its children's spans inside it (children that ran in parallel count once); a `waited` cell's span is waiting, and counts none |
| `total_ns` | time in all: its span, children included, counted only for a group's outermost cell on each demand path, so a group nested in itself counts that time once |
| `alloc_bytes`, `peak_bytes`, `retained_bytes`, `allocs` | as in the events |
| `executions` | cells that ran (`executed`, `cutoff`) |
| `hits` | cells found green: `reused`, `verified`, `durable` |
| `waits` | cells another frame owned: `waited`, `recomputed` (contention) |

## Other lines

- `{"exit_status":…,"diagnostics":…}`: the compile's exit status and its diagnostics.
- `{"error":…}`: the compile did not happen.
- `{"restart":…}`: the build is traced again from the start (a lost daemon, the in-process
  fallback); what came before belongs to the abandoned run.
- `{"build":n,"label":…}`, `{"watching":[…]}`: with `--watch`, a build starts; the files it waits on.
- `{"watching":…,"pid":…}`, `{"request":n,"label":…}`, `{"ended":…}`: with `ctl trace`, the
  attached daemon, each request's start, and the end of the watch.
- `{"note":…}`: a line of text.

## The interactive browser

`--trace-engine=interactive` and `ycxx ctl trace --interactive` open a full-screen browser on the
terminal (`src/trace/browse.cppm`, whose model is `src/trace/view.cppm`). `?` lists its keys.

- **Size.** It draws for the terminal's size and redraws at once when that changes (`SIGWINCH`).
  The summary at the top wraps onto more lines on a narrow screen. Autowrap is off while it runs,
  so a glyph the terminal draws wider than one column is cut at the edge instead of scrolling the
  screen.
- **Mouse** (#635). It asks for SGR mouse reports (modes 1000, 1002 and 1006), which modern
  terminals and tmux support.
  - Click a row to select it. Click its `▸`/`▾`, or double-click the row, to fold or open it.
  - Click a column header (`cell`, `time`, `alloc`, `retains`) to sort by it. Click it again to
    go back to start order. The `alloc` and `retains` columns sum over the cell's subtree.
  - Click `tree` or `timeline` in the status line to switch views. Click the pane to give it
    the keys: arrows, `j`/`k`, PgUp/PgDn and `g`/`G` then scroll it, and `h`, `←` or Escape give
    them back to the tree.
  - The wheel scrolls whatever is under it: the tree, the pane, or the timeline's cursor (Shift
    for bigger steps).
  - Drag the divider (`│`, marked `⇔` in the title row) to resize the pane; `<` and `>` move it
    from the keyboard. It keeps its share of the width when the terminal is resized.
  - In the timeline, click a slice to put the cursor there, or double-click it to open its cell
    in the tree.
  - While the browser holds the mouse, the terminal's own text selection needs the terminal's
    bypass modifier (often Shift; see your terminal's settings).
  - Checked by hand on macOS: Terminal.app; Terminal.app running tmux 3.8 with `mouse on`; and
    Ghostty. Clicks on tabs, rows and headers, double clicks and the wheel all worked. The pty
    test (`daemon.trace_tui`) sends the SGR bytes a terminal sends, drags included, on every
    host CI runs.
- **Histogram** (#637). `b`, or a click on the `histogram` tab, shows bar charts of what groups
  of cells cost. The metrics are the JSON summary's (below): self time, time in all, allocated,
  peak, retained, allocations, runs, cache hits and waits.
  - `1`–`9`, or a click on a switch, shows or hides a metric. Each shown metric is a column of bars.
  - `c`, or a click, groups by query kind, by source file, or by the children of the selected cell
    (each over its subtree, one bar per child even when two share a name). `i` keeps only the
    selected cell's subtree.
  - Each column of bars shares one scale. Metrics that do not fit the width are named in the
    grouping line.
  - `s`, or a click on a column's header, sorts by that metric.
  - `⏎`, or a click on a bar, shows its cells in the tree. A kind adds `kind:Name` to the filter,
    and a file adds `at:=path` (that path exactly), keeping the words already typed. A child
    becomes the selection. With `i`, the selected cell stays selected; the filter itself applies
    to the whole tree.
  - The tree's filter applies to the histogram too.
- **Source** (#638). The pane names the cell's `source` (`a.cpp:3:1–7:1`). `S` adds a source
  column. `p` copies `path:line:col`. The filter's `at:a.cpp:12` keeps the cells whose span
  touches line 12.
- **Copy** (#636). What the browser copies goes to the system clipboard. The status line says
  what was copied, and how.
  - `y` copies the selected cell as the pane shows it: its kind and key, outcome, times, memory,
    errors and value (L1 or L2 IR, or instructions).
  - `Y` copies its whole subtree, folded or filtered parts included, as `--trace-engine` prints
    it: one line per cell, with time, worker and memory.
  - Drag over tree rows to copy them as shown (folded rows stay folded), in the same format,
    indented from the shallowest. Drag over the pane's text to copy exactly the characters
    covered. The copy happens when the button comes up, as with a terminal's own selection.
  - `V` selects from the keyboard: rows from the selected one in the tree, or whole lines from
    the top one shown when the pane has the keys. `j`/`k` extend it, `y` copies it and Escape
    drops it.
  - **How it reaches the clipboard.**
    - The browser always writes OSC 52 (`ESC ] 52 ; c ; <base64> BEL`), which works over SSH in
      the terminals that support it (iTerm2 once clipboard access is allowed in its settings, Ghostty, kitty, WezTerm, foot, xterm with
      `allowWindowOps`). Terminal.app ignores it.
    - Inside tmux it also writes tmux's passthrough form. tmux itself takes an application's
      OSC 52 only with `set -g set-clipboard on`, and passes the passthrough form on only with
      `set -g allow-passthrough on`. Its defaults (`external`, `off`) let neither through.
    - A local session (no `SSH_CONNECTION` or `SSH_TTY`) also runs the first of `pbcopy`,
      `wl-copy` (Wayland), `xclip` and `xsel` (X) on the `PATH`. That covers Terminal.app and
      tmux's defaults.
    - `YCXX_CLIPBOARD` replaces that command (a shell command that reads the text on its
      standard input). `YCXX_CLIPBOARD=osc52` turns the command off.
- **Leaving.** `q` gives the terminal back as it was: the main screen, the cursor, autowrap on,
  mouse reports off, cooked mode. So do `SIGTERM`, `SIGHUP`, `SIGINT` and `SIGQUIT`, and a crash
  (`SIGABRT`, `SIGSEGV`, `SIGBUS`, `SIGFPE`, `SIGILL`); after restoring, the signal takes its
  course.

## The daemon's memory

`ycxx ctl status` prints `memory_current`, `memory_peak`, `memory_allocated` and `allocations`
(the process's bytes now, their high-water mark, bytes ever allocated and how many allocations),
`memory_retained` (what the engine's cells retain) and `memory_unattributed` (the rest of what
is allocated now), and a `retained_by_kind: <kind> <bytes> bytes in <cells> cells` line per kind,
most first.
