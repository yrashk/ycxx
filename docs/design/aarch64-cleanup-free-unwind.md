# Cleanup-free AArch64 Darwin unwind emission

The Darwin front end reuses the typed cleanup-free function proof from the Linux slice.
It certifies ordinary namespace functions over scalar signatures with no hidden lifetime
cleanup or native throw expression. Emission excludes the entry point, coalesced definitions,
and functions with inline declarations, written `noexcept`, assembly labels, or static guards.

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
system unwinder to walk the same records. Throw expressions, catch handlers, destructor
personalities and AArch64 cleanup paths remain in [#648](https://github.com/yrashk/ycxx/issues/648).
This slice is tracked in [#737](https://github.com/yrashk/ycxx/issues/737).
