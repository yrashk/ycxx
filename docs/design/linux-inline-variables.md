# Constant-initialized inline variables in x86-64 ELF

An externally linked inline variable with equivalent definitions in multiple translation units
denotes one object with one address. An earlier declaration can mark the variable inline even
when its definition omits the specifier. Namespace-scope `constexpr` alone does not imply
inline. The front end accumulates actual inline declarations by entity before deduplicating
declarations for symbol emission. [C++ draft dcl.inline](https://eel.is/c++draft/dcl.inline)
· [basic.link](https://eel.is/c++draft/basic.link) · [basic.def.odr](https://eel.is/c++draft/basic.def.odr)
· [dcl.constexpr](https://eel.is/c++draft/dcl.constexpr)

Astra recommended reusing ELF weak symbol selection for ordinary external namespace variables
whose initial values fit completely in canonical bytes or zero-fill. The definition keeps its
usual `.rodata`, `.data`, or `.bss` placement and alignment. A runtime initializer or destructor,
or any relocation in the variable's initial value, keeps the definition outside this slice.
This covers mutation of a shared writable object and references to that object through existing
static address relocations. It leaves local symbols and undefined references with their existing
bindings. [C++ draft basic.start.static](https://eel.is/c++draft/basic.start.static)
· [ELF gABI, Symbol Binding](https://gabi.xinuos.com/elf/05-symtab.html#symbol-binding)

An explicit full variable-template specialization qualifies when its own declarations make it
inline and all template arguments are builtin types. The primary template's specifiers do not
set a full specialization's inline status. This admits the pinned libycxx floating-format
records for `float`, `double`, and `long double`. [C++ draft temp.expl.spec](https://eel.is/c++draft/temp.expl.spec)

Dynamic inline initialization remains separate. COMDAT group emission and selection, local
statics, general variable-template instantiations, and broader inline variable definitions
remain in the ELF follow-up. [#368](https://github.com/yrashk/ycxx/issues/368)
· [#422](https://github.com/yrashk/ycxx/issues/422) · [#432](https://github.com/yrashk/ycxx/issues/432)
