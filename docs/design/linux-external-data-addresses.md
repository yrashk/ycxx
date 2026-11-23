# Linux x86-64 external data addresses

[#414](https://github.com/yrashk/ycxx/issues/414) covers taking the runtime address of a
default-visible data definition or an external data declaration.

Emit a 64-bit RIP-relative MOV with `R_X86_64_REX_GOTPCRELX` (42) and addend −4. The ELF
symbol keeps its ordinary binding, so system linkers can allocate a GOT entry or relax the
instruction. A separate pointer addition supplies a member or array element's byte offset.
Internal and string-literal addresses use direct RIP-relative LEA. Static pointer initializers
use the absolute relocation with the initializer's byte addend.
[#416](https://github.com/yrashk/ycxx/pull/416)

The native static executable linker resolves the symbol using existing definition precedence.
It checks the canonical seven-byte REX.W MOV encoding, the relocation field and its addend,
and reserves the entire instruction against overlapping fixups. A defined target relaxes to LEA
with the checked signed displacement `S − 4 − P`. A missing weak definition keeps the MOV and
reads a zero-valued slot. [Static GOT design](linux-static-got.md)

Native dynamic data addresses need the executable export and
interposition rules of [#390](https://github.com/yrashk/ycxx/issues/390). Native shared PIC
remains under [#391](https://github.com/yrashk/ycxx/issues/391). Reference storage and ELF
function symbol metadata follow [#367](https://github.com/yrashk/ycxx/issues/367) and
[#428](https://github.com/yrashk/ycxx/pull/428).

Verification covers same-TU and cross-TU addresses, BSS, data, nonzero offsets, mutation through
pointers, archive member extraction, and malformed relocation shapes. Native and system linker
tests exercise definitions from ycxx objects and host objects. A system DSO preemption test
checks that the object form leaves binding to the system linker and loader.

## Function addresses

The same relocation applies when `&f`, function-to-pointer conversion or a reference binding
forms a runtime address for an ordinary default-visible function, including a definition in the
same translation unit. Use the entity selected by overload resolution and retain its ELF symbol
name through relocation. Same-named references follow the system linker's function-address
rules. Two aliases for one body may bind independently. Direct calls keep `R_X86_64_PLT32`.

The optimization unit fingerprint includes the GOT symbol IDs used by address expressions.
The native static linker applies the same checked relaxation after choosing the definition from
objects and archives. An unresolved weak function address uses a zero-valued static GOT slot.
Native dynamic imports and special bindings such as IFUNC remain with [#390](https://github.com/yrashk/ycxx/issues/390).
[#407](https://github.com/yrashk/ycxx/issues/407)

Astra reviewed producer GOT semantics and native static relaxation for both data and ordinary
function addresses.
[AMD64 psABI §§4.4.1, 5.2, B.2](https://gitlab.com/x86-psABIs/x86-64-ABI/-/jobs/artifacts/master/raw/x86-64-ABI/abi.pdf?job=build)
· [ELF gABI symbol table](https://gabi.xinuos.com/elf/05-symtab.html)
