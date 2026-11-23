# Explicit weak function definitions on Linux x86-64

GNU `[[gnu::weak]]` and `__attribute__((weak))`, including the reserved spelling
`[[__gnu__::__weak__]]`, give an ordinary external namespace-scope function
definition weak binding in ELF. This covers a qualified definition such as the
pinned libycxx implementation of `std::is_debugger_present`.
[#937](https://github.com/yrashk/ycxx/issues/937)

Record the written attribute on each eligible source declaration and combine it
by entity across the translation unit. Apply it to ordinary non-inline
definitions emitted in that unit. The existing inline and template paths keep
their own binding rules. An explicit GNU visibility attribute remains independent
of binding. Namespace visibility is tracked in
[#940](https://github.com/yrashk/ycxx/issues/940).

The existing ELF reader, writer and resolver carry the weak binding. One
ordinary strong definition takes precedence in either input order, and two
strong definitions still produce a duplicate-symbol diagnostic. A cleanup-free
unwind frame addresses its own code through a local symbol.
[ELF gABI symbol binding](https://gabi.xinuos.com/elf/05-symtab.html#symbol-binding)

Weak undefined references, other GNU weak extensions and weak hosted handlers
remain outside this profile. Darwin parity is tracked in
[#659](https://github.com/yrashk/ycxx/issues/659).
