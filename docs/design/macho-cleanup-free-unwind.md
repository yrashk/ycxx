# Cleanup-free DWARF frames in Mach-O

The first slice of [#72](https://github.com/yrashk/ycxx/issues/72) preserves call frame
information from `__TEXT,__eh_frame` in an arm64 Mach-O link. A hosted unwinder can walk a
frame whose register recovery requires these records. The linker keeps `.eh_frame` data in
read-only `__TEXT` and resolves its addresses before signing the image.

Inputs use 32-bit DWARF record lengths and a version 1 `zR` CIE. The supported CIE describes
AArch64's return address register and an encoded PC-relative pointer. Each FDE points to an
earlier CIE in the same contribution. Its initial location has a Mach-O `SUBTRACTOR` and
`UNSIGNED` relocation pair of the encoding's width. The pair must denote an input text symbol
minus the FDE's location field. The covered range stays inside that input's text. A frame bound
to a weak text symbol stays outside this profile because ordinary symbol resolution could
retarget its relocation to a different definition.

Contributions retain their record order and are concatenated on four-byte boundaries. A zero
length ends an input contribution. The linker keeps one zero-length terminator after the last
contribution so a preceding object's terminator cannot hide later frames. CIE pointers are
record-relative and retain their values. The existing address-difference relocation computes
each PC encoding from the final addresses, including when multiple objects contribute code and
frames. The usual signed output path covers the new section.

The linker retains the existing treatment of sections outside this profile. Compact unwind
tables and frames with personalities, LSDA pointers, or other encodings are tracked in #72.
AArch64 frame emission and exception lowering are tracked in #648. The binary layouts follow
the DWARF Call Frame Information format, the Itanium C++ ABI exception handling frame format,
and Apple's published Mach-O format and ARM64 relocation definitions.
