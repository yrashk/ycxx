# Cleanup-free AArch64 Darwin unwind emission

The Darwin front end reuses the typed cleanup-free function proof from the Linux slice.
It certifies ordinary namespace functions with fixed scalar parameters and no hidden lifetime
cleanup or native throw expression. Emission excludes the entry point, coalesced definitions,
and functions with inline declarations, written `noexcept`, assembly labels, or static guards.

For a non-union aggregate with scalar fields, no bases, and trivial destruction, the proof
visits the selected initializer for each field. The replayed default can read an earlier field
through the construction receiver. An explicit initializer suppresses the corresponding default
before this walk. A materialized temporary of the same aggregate shape is also eligible when its
lifetime ends with the full-expression. [dcl.init.aggr], [class.mem.general], [class.temporary].
[#849](https://github.com/yrashk/ycxx/issues/849)

A result can be a nonempty, flat scalar aggregate that is trivial for calls. A braced result or
the trivial implicit copy of a named result has no exceptional cleanup ([stmt.return], [except.ctor]).
The target ABI independently validates and lowers the result, including an indirect result in X8.
Calls in the body still use the existing scalar signature proof. [#853](https://github.com/yrashk/ycxx/issues/853)

The arm64 emitter records frame transitions as it emits the saves and restores. At entry,
the CFA is the incoming SP and the return address is in X30. After the frame-record store,
the CFA is SP+16, X29 is saved at CFA−16, and X30 is saved at CFA−8. The next instruction
makes X29 the CFA base with the same offset. Stack allocations and probes below the frame
record preserve that rule. The post-indexed frame-record load restores the entry rules before
`ret`. Any later block in byte order reinstates the frame-pointer rules at its first instruction.

Object assembly gives certified functions one FDE each and shares a version 1 `zR` CIE. The
PC-relative location field names the function's own text symbol and subtracts the
address of the field. It uses the existing Mach-O `SUBTRACTOR` and `UNSIGNED` representation.
The frame range, CIE pointer and row offsets describe the emitted bytes. The native linker
keeps them under the [cleanup-free Mach-O profile](macho-cleanup-free-unwind.md).

The first profile covers exception propagation through an ordinary YCXX-compiled frame in a
program linked with the hosted system toolchain. A program linked by YCXX can use the Darwin
system unwinder to walk the same records. [Hosted cleanup](aarch64-hosted-cleanup.md) extends
the object profile with a personality and a cleanup-only table. Throw expressions and catch
handlers are tracked in [#648](https://github.com/yrashk/ycxx/issues/648).
This slice is tracked in [#737](https://github.com/yrashk/ycxx/issues/737).
