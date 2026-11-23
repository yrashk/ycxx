# Linux x86-64 static GOT

The fixed-address ELF executable linker gives `R_X86_64_GOTPCREL` an aligned
eight-byte slot. It patches the relocation's four-byte field with the checked
signed displacement `GOT(S) + A - P`, preserving the instruction and addend.
The slot contains the selected definition's address or zero for an unresolved
weak symbol. Strong definitions and archive extraction use the existing rules.

Slots are assigned in relocation order after archive and COMDAT selection and
placed in a read-only load segment. References to the same nonlocal name share
a slot. A local slot belongs to its input object and symbol index. Slot addresses
and values use the final section layout.

Canonical `R_X86_64_REX_GOTPCRELX` MOV instructions with a selected definition
still relax to LEA. When the weak symbol remains unresolved, the validated MOV
uses the same zero-valued slot as a plain GOTPCREL reference. Both forms of the
canonical MOV retain whole-instruction overlap checks. Astra reviewed the
fixed-address GOT design.

[#979](https://github.com/yrashk/ycxx/issues/979) · [AMD64 psABI §§4.4.1, 5.2, B.2](https://gitlab.com/x86-psABIs/x86-64-ABI/-/jobs/artifacts/master/raw/x86-64-ABI/abi.pdf?job=build)
