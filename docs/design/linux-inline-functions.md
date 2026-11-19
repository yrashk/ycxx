# External inline functions in x86-64 ELF

An externally linked inline function can have equivalent definitions in multiple translation
units and has one address. A reachable declaration can make the entity inline even if the
definition omits `inline`. A `constexpr` function and an ordinary member defined inside its class
in the global module are implicitly inline. The front end already records those flags on
individual declarations. The translation unit accumulates them by entity before deduplicating
its declarations for symbol emission. [C++ draft dcl.inline](https://eel.is/c++draft/dcl.inline)
· [dcl.constexpr](https://eel.is/c++draft/dcl.constexpr)
· [basic.def.odr](https://eel.is/c++draft/basic.def.odr)

In the first slice, an emitted, ordinary external inline function has an `STB_WEAK` symbol with
its external linkage name in the ELF object. The linker resolves each nonlocal call (`R_X86_64_PLT32`)
and each saved address (`R_X86_64_64`) by that symbol name, including relocations in a unit that
defined a second copy. It selects the first weak definition in operand order, gives precedence
to a strong definition, and still diagnoses two strong definitions. Undefined references retain
their normal binding and can extract a weak definition from an indexed archive. Internal inline
functions remain local symbols. [ELF gABI, Symbol Binding](https://gabi.xinuos.com/elf/05-symtab.html#symbol-binding)

Astra verified the resolver and archive behavior and used GCC 16.2 and Clang 23 as black-box
oracles for mixed-object links. The Itanium C++ ABI specifies a COMDAT group named by the function's
mangled symbol. Its vague-linkage examples describe standalone weak definitions as an interim
representation that can link with grouped definitions. Duplicate bodies can still take up space
in the image. [Itanium C++ ABI §5.2.1](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vague-inline)
· [ABI examples, Vague Linkage](https://itanium-cxx-abi.github.io/cxx-abi/abi-examples.html#vague)

Group emission and selection, emission on ODR-use, and identity for local statics, guards,
templates, closures, and inline variables remain in the ELF follow-up. Dynamic initialization
of inline variables, the Mach-O implementation, internal-linkage mangling, and taking an external
function's address at run time have separate issues.
[#422](https://github.com/yrashk/ycxx/issues/422) · [#368](https://github.com/yrashk/ycxx/issues/368)
· [#124](https://github.com/yrashk/ycxx/issues/124) · [#193](https://github.com/yrashk/ycxx/issues/193)
· [#407](https://github.com/yrashk/ycxx/issues/407)
