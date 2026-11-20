# Linux ELF COMDAT input

[#422](https://github.com/yrashk/ycxx/issues/422) adds `GRP_COMDAT` group selection to
x86-64 static executable links made from explicit inputs. The first group with a signature
wins in input order. The signature is the name of the referenced ELF symbol table entry.
Plain definitions do not reserve signatures. Apply the existing weak and strong binding rules
to the surviving definitions. [ELF gABI §3.9](https://gabi.xinuos.com/elf/03-sheader.html#section-groups)

The ET_REL reader validates each descriptor's linked table, signature and flags before
normalizing section indices. Every member has `SHF_GROUP`, follows its descriptor and
belongs to exactly one group. The reader retains already supported allocatable section
kinds with conventional names or grouped name suffixes. An `SHT_RELA` section and its
target must have the same owner. A relocation may reference a grouped local or section
symbol only from another member of its group. The reader validates those rules in every
candidate, including groups that will later lose selection.

Select the live sections before counting `.init_array` slots, choosing startup symbols,
registering definitions, laying out segments or applying relocations. Retained relocations
to nonlocal symbols defined in losing groups resolve by name, with ordinary undefined
symbol and weak-reference behavior when no definition survives. A reference that existed
only in discarded sections does not need a final relocation. The reader retains the
original undefined table entries. They can create archive demand even when every use was
discarded, so archive admission needs its own rules and tests.

Diagnose grouped inputs with dynamic providers or archives, including a grouped archive
member when it is selected. Unselected archive members stay unread. Native archive
production and ELF object emission diagnose grouped objects. Group emission and remaining
COMDAT link modes stay in [#422](https://github.com/yrashk/ycxx/issues/422). Mach-O remains
in [#124](https://github.com/yrashk/ycxx/issues/124).

Astra independently reviewed the ABI and verified GNU ld and LLD outcomes for group order,
whole-group removal, undefined symbols, archive demand and plain strong definitions.
[Itanium C++ ABI §5.2.1](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vague-inline)
