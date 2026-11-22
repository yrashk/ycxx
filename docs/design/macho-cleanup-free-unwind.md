# DWARF frames in Mach-O

The first slice of [#72](https://github.com/yrashk/ycxx/issues/72) preserves call frame
information from `__TEXT,__eh_frame` in an arm64 Mach-O link. A hosted unwinder can walk a
frame whose register recovery requires these records. The linker keeps `.eh_frame` data in
read-only `__TEXT` and resolves its addresses before signing the image.

Inputs use four- or eight-byte section alignment, 32-bit DWARF record lengths and a version 1
`zR` CIE. The supported CIE describes AArch64's return address register and an encoded
PC-relative pointer. Each FDE points to an
earlier CIE in the same contribution. Its initial location has a Mach-O `SUBTRACTOR` and
`UNSIGNED` relocation pair of the encoding's width and uses the pair validation in
[#702](https://github.com/yrashk/ycxx/issues/702). The pair must denote an input text symbol
minus the FDE's location field. The covered range stays inside that input's text. A frame bound
to a weak text symbol stays outside this profile because ordinary symbol resolution could
retarget its relocation to a different definition.

Contributions retain their record order and are concatenated on four-byte boundaries. A zero
length ends an input contribution. The linker keeps one zero-length terminator after the last
contribution so a preceding object's terminator cannot hide later frames. CIE pointers are
record-relative and retain their values. The existing address-difference relocation computes
each PC encoding from the final addresses, including when multiple objects contribute code and
frames. The linker preserves the CFI instruction stream as supplied. The usual signed output
path covers the new section.

AArch64 [cleanup-free frame emission](aarch64-cleanup-free-unwind.md) uses this profile.

## Cleanup-only frames

[#783](https://github.com/yrashk/ycxx/issues/783) adds the version 1 `zPLR` CIE profile. Its
personality field is an indirect signed PC-relative word to the GOT cell for the Darwin symbol
`___gxx_personality_v0`. The object model carries a zero addend for that field. The FDE
location and LSDA fields use pointer-sized PC-relative differences. The linker resolves
both against the exact fields after placement, using a local anchor in the same frame section.

The LSDA points to a local table in the same object's `__TEXT,__gcc_except_tab`. The table omits
LPStart and types. Its ULEB128 call-site rows have action zero, nonoverlapping ranges within the
FDE's text extent and any landing pad addresses aligned within the function. A landing pad lies
outside its own protected range. The linker diagnoses an invalid recognized cleanup frame.
The `unit.link` controls inspect both metadata profiles after placement and exercise a cleanup
bridge from a platform-linked typed handler on arm64 macOS.

The GOT wire format is tracked by [#782](https://github.com/yrashk/ycxx/issues/782) and source
lowering by [#781](https://github.com/yrashk/ycxx/issues/781). Compact unwind and other
encodings are tracked by [#72](https://github.com/yrashk/ycxx/issues/72). General catch tables
are tracked by [#648](https://github.com/yrashk/ycxx/issues/648). The binary layouts follow
the DWARF Call Frame Information format, the Itanium C++ ABI exception handling frame format,
and Apple's published Mach-O format and ARM64 relocation definitions.
