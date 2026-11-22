# Hosted cleanup on AArch64 Darwin

The same typed proof as [the Linux one-owner case](linux-hosted-catch-all.md#hosted-local-cleanup-663)
admits an ordinary fixed-scalar `void` function that initializes one automatic aggregate
with a known nonthrowing destructor and calls a direct external `void()` provider. Both
continuations destroy the owner. The exceptional continuation resumes the original exception
through `_Unwind_Resume`. [except.ctor], [class.dtor] and the Itanium C++ exception ABI.

An ordinary potentially throwing call in a selected aggregate default member initializer
requires a certified cleanup edge when a nontrivial automatic local is live.
[#850](https://github.com/yrashk/ycxx/issues/850)

L2 carries the normal and exceptional successors of the protected call. At the AArch64 landing
pad the unwinder supplies its exception header in X0 and selector in X1. The register allocator
saves the header across destruction. The physical emitter records the protected BL and the
first instruction of the landing pad, including when that pad follows a normal return in layout
order. The FDE uses the same actual frame-pointer and return-address rows as the
[cleanup-free profile](aarch64-cleanup-free-unwind.md).

The Mach-O object gives the cleanup FDE a version 1 `zPLR` CIE. The personality points to the
GOT entry for `__gxx_personality_v0`, and the FDE names the cleanup table in
`__TEXT,__gcc_except_tab` by a PC-relative address difference. The table covers the four-byte
call with a landing pad and action zero. The surrounding ranges have no landing pad. It omits
the type table and uses the function's initial address as the landing-pad base.

`unit.darwin_local_unwind` checks the object cross-host and runs the independent #663 peer on
arm64 macOS. It verifies one destruction and the original exception on the exceptional exit,
and one destruction on the successful exit. Native cleanup metadata preservation is tracked in
[#783](https://github.com/yrashk/ycxx/issues/783). This source slice is [#781](https://github.com/yrashk/ycxx/issues/781).
