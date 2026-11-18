# ycxx status

What is true of the code today. Work in progress and its owners are tracked in
[GitHub issues](https://github.com/yrashk/ycxx/issues).

## Bootstrapping (2026-11-17)

- Charter written: [README.md](README.md), [DECISIONS.md](DECISIONS.md) (scope, provenance,
  bootstrap, architecture direction, process, coordination), [CLAUDE.md](CLAUDE.md) (agent guide).
- libycxx added as a submodule at `deps/libycxx`.
- No compiler code yet. The query engine design (`docs/design/query-engine.md`) is the first
  piece of work. Everything else waits for it.

## Bootstrap workarounds

Places where ycxx's sources avoid or work around a C++26 feature because GCC 16 lacks it or
miscompiles it (DECISIONS §3.2). Each is removed once ycxx compiles itself. None yet.

## Milestones

| | Milestone | Done when |
| --- | --- | --- |
| M0 | Foundations | Query-engine, AST and determinism designs accepted; build, CI and test driver run; draft examples extracted |
| M1 | Preprocessor | `-E` matches Clang's on the oracle corpus; incremental macro-dependency caching |
| M2 | Parser | The full grammar parses libycxx, ycxx and the real-world corpus |
| M3 | Semantic analysis | Accept/reject agrees with the draft examples and the oracles |
| M4 | IR and interpreter | The own suite and libycxx tests run in the interpreter |
| M5 | AArch64 Mach-O | Native objects and our own linker; mixed-ABI tests with Clang pass |
| M6 | x86-64 ELF | Same on Linux |
| M7 | libycxx under ycxx | libycxx's suite passes, compiled by ycxx |
| M8 | Self-hosting | Stage 2 equals stage 3; the GCC-built bootstrap anchor is released (DECISIONS §3.5) |
