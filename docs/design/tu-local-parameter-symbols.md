# Function symbols with unnamed-namespace parameter types

Each translation unit has its own unnamed namespace. The parser already gives these namespaces
distinct identities, while the Itanium spelling uses the same namespace component in each unit.
Astra recommended carrying that ownership through native symbol binding for ordinary C++
namespace functions. Their source linkage, entity identities and mangled spellings stay intact.
[Issue #582](https://github.com/yrashk/ycxx/issues/582) · [`[namespace.unnamed]`](https://eel.is/c++draft/namespace.unnamed) · [`[basic.link]`](https://eel.is/c++draft/basic.link) · [Itanium C++ ABI §5.1](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#mangling)

Read the function's canonical adjusted parameter types. Follow a class or enum's nominal
owner chain to its namespace. Follow pointer, reference, array and member-pointer
components and the return and parameters of a function type nested in a parameter.
Follow concrete class-template type and template arguments, including packs, because
the mangler encodes them in the type name. The outer function's ordinary mangled name
omits its return type.

The pipeline uses local binding for affected definitions on ELF and Mach-O, including
inline definitions. The same decision controls direct ELF addressing. A call or address
requiring an affected function with no emitted definition in the unit diagnoses the
declaration before target-specific assembly. An unused prototype requires no symbol.
[ELF symbol binding](https://gabi.xinuos.com/elf/05-symtab.html#symbol-binding)

C language linkage, explicitly assigned asm labels, function template specializations and
scoped forms outside the current mangler retain their existing rules. This decision covers
object emission for non-module translation units.
