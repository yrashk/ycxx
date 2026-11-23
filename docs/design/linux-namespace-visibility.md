# GNU namespace visibility for ordinary ELF functions

On Linux x86-64, capture the nearest explicit GNU `hidden` or `default`
visibility of the namespace-definition occurrence that contains an ordinary
external non-inline function declaration. Unannotated namespace blocks inherit
their lexical context. A reopening has its own context. An explicit
function attribute takes precedence over a consistent inherited setting.
This follows Astra's review of the pinned libycxx debugger declaration and
the GCC and Clang oracle results.
[#940](https://github.com/yrashk/ycxx/issues/940)

The per-declaration visibility facts reach the translation unit with the
canonical function entity resolved by ordinary lookup. This includes the
inline namespace in the libycxx header. An unannotated qualified definition
and a referenced declaration then use the same ELF visibility. The preprocessed
namespace header is part of the translation unit analysis. Its visibility
can change that symbol even when the function tokens keep the same item identity.
[ELF gABI](https://gabi.xinuos.com/elf/05-symtab.html#symbol-visibility)

Mixed inherited `hidden` and `default` redeclarations have differing GNU
compiler behavior and retain ycxx's existing direct-attribute rules. General
namespace attribute support is tracked in
[#190](https://github.com/yrashk/ycxx/issues/190) and Darwin parity in
[#659](https://github.com/yrashk/ycxx/issues/659).
