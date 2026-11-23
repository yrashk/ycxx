# Linux x86-64 borrowed typed downcasts

`dynamic_cast<D*>(p)` accepts a borrowed `B*` when `D` has one public nonvirtual primary
base `B` at offset zero. The initial profile uses two complete, named, externally linked,
default-visible, fieldless classes outside templates. Each declares one public, non-pure,
non-inline virtual `int` member with no arguments. The member in `D` overrides the member in
`B`. Their out-of-line definitions in the native peer make those members the respective
key functions and supply both classes' RTTI.

Semantic analysis checks the operand and target categories, completeness, polymorphism and
pointee cv. The object pass checks the two classes, the base edge and the key declarations
after analysing the full unit. A legal edit can remove the derived override, add a definition
of a key, or change visibility after the cast. The object pass then diagnoses the unsupported
cast. The front-end query reads the full source and records the object eligibility result
alongside the typed cast. Restoring the eligible declarations restores the original imports.

For a non-null operand, call `__dynamic_cast` with C language linkage, the original source
subobject pointer, the addresses of the two static types' RTTI objects, and signed
`ptrdiff_t(0)`. The hint describes `B` within `D`. A peer can pass a different `B` subobject
of a larger most-derived object. The runtime locates the corresponding `D` subobject or
returns null. The descriptor symbols use canonical unqualified type encodings and the
ordinary [ELF external data relocation](linux-external-data-addresses.md). The helper uses
the ordinary direct-call relocation.

The cast evaluates the operand once and branches before the runtime use. The null edge
returns a typed null in both evaluation modes. A pointer already known to be null can
produce that result without any runtime references. The hosted peer driver supplies the
runtime and resolves the imports. Broader typed casts, class `typeid`, and non-null
constant evaluation follow under [#294]. Native hosted linking remains under [#390].
Darwin parity is tracked in [#659].

Astra reviewed the ownership boundary and call ABI for [#294].
[[expr.dynamic.cast]](https://eel.is/c++draft/expr.dynamic.cast) ·
[Itanium ABI §2.9.7](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#dynamic_cast) ·
[Itanium ABI §5.2.3–4](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vague-vtable)

[#294]: https://github.com/yrashk/ycxx/issues/294
[#390]: https://github.com/yrashk/ycxx/issues/390
[#659]: https://github.com/yrashk/ycxx/issues/659
