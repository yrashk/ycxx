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

Memory that one cell's run allocated and others use (an interned tree, a header's region) counts
for that cell only. Memory no frame allocated (the engine's tables, the session's state) counts
for no cell.

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

## Other lines

- `{"exit_status":…,"diagnostics":…}`: the compile's exit status and its diagnostics.
- `{"error":…}`: the compile did not happen.
- `{"restart":…}`: the build is traced again from the start (a lost daemon, the in-process
  fallback); what came before belongs to the abandoned run.
- `{"build":n,"label":…}`, `{"watching":[…]}`: with `--watch`, a build starts; the files it waits on.
- `{"watching":…,"pid":…}`, `{"request":n,"label":…}`, `{"ended":…}`: with `ctl trace`, the
  attached daemon, each request's start, and the end of the watch.
- `{"note":…}`: a line of text.

## The daemon's memory

`ycxx ctl status` prints `memory_current`, `memory_peak`, `memory_allocated` and `allocations`
(the process's bytes now, their high-water mark, bytes ever allocated and how many allocations),
`memory_retained` (what the engine's cells retain) and `memory_unattributed` (the rest of what
is allocated now), and a `retained_by_kind: <kind> <bytes> bytes in <cells> cells` line per kind,
most first.
