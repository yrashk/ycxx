# Linux x86-64 weak RTTI for borrowed downcasts

Extend the [borrowed typed downcast](linux-borrowed-typed-downcast.md) to the abstract-base
profile established by [#905]. The base `B` is fieldless and declares one public pure virtual
`int f()`. The derived class `D` has one public nonvirtual primary base `B` at offset zero,
one public `int` field, and one directly declared override. Both classes have the default-visible
external ABI. Astra reviewed the ownership split and the mixed-object execution cases. [Design
checkpoint](https://github.com/yrashk/ycxx/issues/294#issuecomment-7086812661)

## Admission

The typed cast keeps its language checks and runtime lowering. The object pass classifies the pair
after analysing the full translation unit. The existing automatic-primary-base certificate must
identify `B` within `D`. Its whole-unit declaration check must leave the pure member and the
derived key as declarations. Use the existing visibility and layout checks for both classes. A
later definition of `D::f` or a visibility-changing redeclaration invalidates this cast profile
in object mode. The earlier two-key profile continues to import both RTTI descriptors.

## Ownership and emission

For a kept runtime reference, define the canonical `_ZTI` for `B` as a weak,
default-visible 16-byte object in `.data.rel.ro`. Its first pointer has an ABS64 relocation to
the runtime's `__class_type_info` vtable with addend 16. Its second pointer has an ABS64 relocation
to the type-name object `_ZTS` for `B`. The descriptor's ELF COMDAT contains the payload and its
RELA section. The name has its own weak, read-only COMDAT and contains the type encoding with
top-level cv removed, followed by NUL. The existing derived-table producer emits this
[grouped descriptor](linux-elf-comdat-input.md) for the same base.
[Itanium ABI §2.9.5 and §5.2.4][abi] · [#891]

Use the canonical source RTTI symbol already named by the cast's code relocations. Keep one
definition and group per base in the object even when multiple casts reference it or an existing
owned table needs the same base descriptor. The cast supplies neither class's vtable. The native
peer owns the derived key, table and descriptor, and provides access to `__dynamic_cast` and the
standard RTTI runtime. Front-end ownership facts are derived from the completed unit. The object
emits the weak descriptor only when a retained reference or owned table requires it.

The existing lowering evaluates the operand once, returns a typed null on the null edge, and
passes the original subobject pointer, the two descriptor addresses and signed zero hint on the
live edge. The runtime can find a `D` subobject displaced within a peer's most-derived object.
[[expr.dynamic.cast]](https://eel.is/c++draft/expr.dynamic.cast) · [Itanium ABI §2.9.7][cast-abi]

## Verification

The independent fixture exercises coalescing with two consumers and GCC and Clang peers in either
object order. Inspect the two COMDAT payloads and named relocations, the derived RTTI import, and
the direct runtime call. Check successful primary and displaced casts, a failed cast from a sibling,
cv preservation and the typed-null result. Compare cold and resident outputs across a late local
key definition, a visibility edit and removal of the last cast. Broader ownership and cast forms
remain in [#294]. Darwin parity is tracked in [#659]. [#930]

[abi]: https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vague-rtti
[cast-abi]: https://itanium-cxx-abi.github.io/cxx-abi/abi.html#dynamic_cast
[#294]: https://github.com/yrashk/ycxx/issues/294
[#659]: https://github.com/yrashk/ycxx/issues/659
[#891]: https://github.com/yrashk/ycxx/pull/891
[#905]: https://github.com/yrashk/ycxx/pull/905
[#930]: https://github.com/yrashk/ycxx/issues/930
