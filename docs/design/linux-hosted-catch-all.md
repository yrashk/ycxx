# Hosted Linux x86-64 catch-all object records

Astra reviewed the handler ABI, machine layout, and hosted linking boundary on [#583].
This first stage develops the object records for one protected direct call in a
cleanup-free function. Entry from a source `try` statement requires the separate
landing-pad execution proof. Existing cleanup-free functions retain the call-frame
information from [#572].

## Frame and exception tables

Use separate CIEs for ordinary `zR` frames and handler `zPLR` frames. Each FDE
names the CIE that describes it, regardless of function order. Both describe the
actual emitted frame state, with a signed 32-bit PC-relative initial-location
relocation to a local code anchor. The handler CIE uses an indirect signed 32-bit
PC-relative reference to an aligned local pointer in ELF `.data.rel.ro`. One
`R_X86_64_64` relocation initializes that cell to the hosted
`__gxx_personality_v0` symbol. The FDE's signed 32-bit PC-relative LSDA field
references a local anchor in allocatable, read-only `.gcc_except_table`. The
PC-relative fields have zero relocation addends relative to their local anchors.

The machine emitter supplies function-relative byte ranges in instruction
order. A call-site entry covers the emitted call instruction through the byte
before its return address. A landing entry points to the first pad instruction
in the same function. The LSDA uses the FDE's region start and a table of ULEB128
call-site entries. Exactly one protected call has action index 1. Its action is
a positive filter with one null type-table entry and no next action. Calls in
the same function that may unwind outside the protected range have separate
no-pad entries. Validate ranges, order, and the presence of the single pad
before emitting object records.

## Execution boundary

The L1 exceptional edge carries the exception into the landing path. On System V
AMD64 the personality installs the unwind header in `%rax` and the selector in
`%rdx`. Ordinary control flow returns or branches past the pad. The selected
catch-all path calls `__cxa_begin_catch` with the original unwind header and
`__cxa_end_catch` once before returning. A cleanup selector calls
`_Unwind_Resume` with the original header. Do not derive instruction ranges from
source, IR indices, or assumed displacements.

The hosted final link supplies the personality, catch helpers, and discovery of
the registered frames. The native ycxx ELF executable linker must reject handler
tables until it can supply unwind discovery. Object validation distinguishes the
ordinary code anchor from the personality cell and LSDA targets, including the
unaligned relocation fields inside augmentation data. Typed and nested handlers,
local lifetime cleanup, rethrow, and other targets remain under [#89].

[Itanium Exception Handling ABI](https://itanium-cxx-abi.github.io/cxx-abi/abi-eh.html) ·
[System V AMD64 psABI](https://gitlab.com/x86-psABIs/x86-64-ABI) ·
[DWARF call frame information](https://dwarfstd.org/doc/DWARF5.pdf) ·
[Pinned libycxx personality](https://github.com/yrashk/libycxx/blob/c3315ab3/src/abi/personality.cpp)

[#583]: https://github.com/yrashk/ycxx/issues/583
[#572]: https://github.com/yrashk/ycxx/issues/572
[#89]: https://github.com/yrashk/ycxx/issues/89
