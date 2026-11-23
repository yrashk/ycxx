# Sharing query snapshots and paging engine storage

Both features are opt-in on Linux and macOS. A compiler without these settings uses its normal
in-process memo table and heap-backed arenas.

## Restore source snapshots on another builder

Create separate absolute cache directories for the producer and consumer. Build with the same
ycxx executable and restore the producer's directory through your CI artifact service.

```sh
mkdir -p "$PWD/.ycxx-source-cache"
YCXX_ENGINE_SNAPSHOT_DIR="$PWD/.ycxx-source-cache" ycxx cxx --no-daemon -c src.cpp
```

The current eligible query prepares source for translation phases 1–2, including physical offset
maps and UTF-8 diagnostics. Raw sources from 4 KiB through 8 MiB are eligible, including headers.
The directory stores the source contents in normalized form. Keep artifacts private to builders
with access to those sources. Accept artifacts only from trusted build jobs. The record checksums
detect accidental corruption and the compiler fingerprints detect incompatible inputs. Neither
authenticates the producer.

A record is keyed by the running build's identity and the original source fingerprint.
Linux fingerprints the executable through `/proc/self/exe`. macOS uses the loaded main Mach-O
image's linker UUID. If the running build's identity is unavailable, the compiler computes
normally. Each record also carries the query schema, format version and host layout signature. Use
identical compiler binaries on the same host architecture and ABI. The compiler looks up records
independently of working directory and source path. It rechecks the input fingerprint through
normal query validation before exposing the stored result. Unavailable, stale, incompatible or
corrupt records fall back to normal computation. The reader maps the file read-only and borrows
the normalized character buffers directly while the lexer needs them. Fixed-width offsets,
diagnostics and gap tables use the structural codec.

Publish and restore into distinct cache directories or use atomic rename for existing entries.
Keep a published inode immutable. Replacing its directory entry leaves readers of the old inode
intact. Overwriting or truncating a mapped inode can change results or make the operating system
terminate its reader. The publisher stages a new file and renames it over the old name only after
writing it completely. Old cache entries accumulate as inputs or the compiler binary change.
Assign a size or age limit to the CI artifact and prune between builds. Artifacts should be
disposable. Readers can reuse compatible files in a read-only directory.

This is the source-preparation slice of [#17]. Full daemon checkpoint persistence, interned
AST and semantic state, and automatic engine eviction are still tracked there. The disk view
uses snapshots. Editor-view builds keep unsaved sources out of these artifacts. A resident daemon
must start with these settings in its environment. The examples use `--no-daemon` to make the
process and cache directory explicit.

## Page engine arenas to local storage

```sh
mkdir -p "$PWD/.ycxx-arena-space"
YCXX_ENGINE_PAGING_DIR="$PWD/.ycxx-arena-space" ycxx cxx --no-daemon -c src.cpp
```

Use a private directory on a local disk with room for the engine's arena blocks. A mounted memory
filesystem will consume RAM for the backing store. The engine reserves file space before mapping
a block and removes the directory entry as soon as it opens the file. The mapping holds that
space until the database is destroyed or its process exits. The blocks contain cell and typed
slot records. Transitive allocations such as a slot's string buffer, dependency vectors,
interners, shard indexes and scheduler allocations stay on the heap. An unavailable filesystem,
failed preallocation or unsupported alignment falls back to a heap block.

The OS can page arena blocks as file-backed memory. Paging does not create an artifact and the
temporary files are process-private. Resident memory is workload and OS dependent. Large working
sets under memory pressure are the reason to try it. Measure end-to-end build latency, peak
memory and available disk before enabling it for a workload.

## Measure

```sh
tools/bench-engine-storage build/bin/ycxx --repetitions 7
tools/bench-engine-storage build/bin/ycxx --repetitions 7 --files 128
```

The benchmark uses one syntax-only build per fresh process. It measures heap-only cold execution,
cold snapshot creation, warm import of a copied artifact under a different directory, an edit
after restoring the original snapshot, and paging alone. The compiler, restored artifacts and
sources may all be warm in the OS page cache. The benchmark reports process peak RSS and elapsed
time. It does not drop the OS page cache or force disk reads.

Seven repetitions on Linux x86-64 with an overlay filesystem and ycxx bootstrapped with GCC 16.2
at `-O1` gave the following medians. The single-file workload wrote a 3,766,486-byte artifact
from 1,824,018 source bytes. The 128-file workload wrote 3,780,319 bytes from 1,819,696 source
bytes.

| Mode | One file, ms | One file, peak RSS KiB | 128 files, ms | 128 files, peak RSS KiB |
| --- | ---: | ---: | ---: | ---: |
| Disabled | 24.67 | 14,280 | 19.77 | 10,896 |
| Cold snapshot write | 50.99 | 21,524 | 96.34 | 11,220 |
| Warm snapshot import | 24.29 | 14,640 | 23.02 | 11,028 |
| Changed input after restore | 37.91 | 21,392 | 24.87 | 11,112 |
| Paging only | 27.06 | 13,996 | 21.84 | 10,984 |

Neither option showed a clear improvement in latency or peak RSS on these workloads. Warm import
and disabled timings overlapped. Cold snapshot creation had the highest overhead, especially
across many files. Leave both features disabled for similar workloads unless measurements show a
benefit on your builders.

[#17]: https://github.com/yrashk/ycxx/issues/17
