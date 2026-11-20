# Linux x86-64 external data addresses

[#414](https://github.com/yrashk/ycxx/issues/414) covers taking the runtime address of a
default-visible data definition or an external data declaration. Use the translation unit's
entity declarations to distinguish data from functions. Include the referenced data IDs in the
optimization unit fingerprint alongside directly addressable symbols. External function addresses
remain under [#407](https://github.com/yrashk/ycxx/issues/407).

Emit a 64-bit RIP-relative MOV with `R_X86_64_REX_GOTPCRELX` (42) and addend −4. The ELF
symbol keeps its ordinary binding, so system linkers can allocate a GOT entry or relax the
instruction. A separate pointer addition supplies a member or array element's byte offset.
Internal and string-literal addresses use direct RIP-relative LEA. Static pointer initializers
use the absolute relocation with the initializer's byte addend.
[#416](https://github.com/yrashk/ycxx/pull/416)

The native static executable linker resolves the symbol using existing definition precedence.
It checks the canonical seven-byte REX.W MOV encoding, the relocation field and its addend,
and reserves the entire instruction against overlapping fixups. It then rewrites the opcode to
LEA and applies the checked signed displacement `S − 4 − P`. A missing weak definition needs
a GOT entry with value zero. Native dynamic data addresses need the executable export and
interposition rules of [#390](https://github.com/yrashk/ycxx/issues/390). Native shared PIC
remains under [#391](https://github.com/yrashk/ycxx/issues/391). Reference storage and ELF
function symbol metadata follow [#367](https://github.com/yrashk/ycxx/issues/367) and
[#428](https://github.com/yrashk/ycxx/pull/428).

Verification covers same-TU and cross-TU addresses, BSS, data, nonzero offsets, mutation through
pointers, archive member extraction, and malformed relocation shapes. Native and system linker
tests exercise definitions from ycxx objects and host objects. A system DSO preemption test
checks that the object form leaves binding to the system linker and loader.

Astra reviewed the distinction between producer GOT semantics and native static relaxation.
[AMD64 psABI §§4.4, B.2](https://gitlab.com/x86-psABIs/x86-64-ABI/-/jobs/artifacts/master/raw/x86-64-ABI/abi.pdf?job=build)
· [ELF gABI symbol table](https://gabi.xinuos.com/elf/05-symtab.html)
