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

The source handler calls a resolved external function with its definition in a
separate translation unit. That provider supplies unwind coverage for frames
between the throw and the call site. Same-translation-unit unwind coverage
continues under [#89].

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

## Hosted local cleanup (#663)

A strong ordinary void function with scalar parameters can keep one aggregate
local with a known nonthrowing destructor live across a direct external
`void()` call. The typed body certifies a single initialized automatic
declaration followed by the call, and the initializer uses scalar operations
without calls or temporary class objects. The provider supplies the frames
from its throw site to ycxx's call site.

L1 emits an `invoke` at the call. Its normal path runs the existing scope
exit, and the exceptional path runs the same local destruction before
`resume` ([except.ctor], [class.dtor]). The cleanup-only LSDA uses the existing
hosted personality and FDE. The protected call-site row names a landing pad
with action zero and the LSDA omits the type and action tables. The landing
pad preserves the incoming unwind header from RAX across the destructor
call and passes it to `_Unwind_Resume`. No catch entry or exit occurs on this
path ([except.handle]). The hosted final link and native linker boundary
are the same as for the catch-all subset. Broader exceptional lifetime
cleanup and handlers remain tracked in [#89].

## Hosted union constructor cleanup (#738)

On Linux x86-64, a nondelegating no-argument union constructor can protect one
direct external `void()` call after its selected aggregate member is initialized.
The member's initializer consists of scalar expressions and can include one
direct external `int()` call. The selected member's destructor must be
nonthrowing. Both providers are defined in separate translation units.

The compiler records the member's address after its initializer completes. The
exceptional edge of the constructor body call destroys that member and resumes
the exception ([class.base.init], [except.ctor]). The earlier initializer call
has a no-pad LSDA entry because the member has not completed initialization.
Normal construction leaves the selected member live for the union's ordinary
lifetime rules. A same-unit caller whose whole body constructs one such automatic
union supplies a cleanup-free frame. Its destructor runs only if construction
succeeds. The hosted peer supplies the exception runtime and the handler.

The wider lifetime model and Darwin parity are tracked in [#89] and [#648].

[#648]: https://github.com/yrashk/ycxx/issues/648
