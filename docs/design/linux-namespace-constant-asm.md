# Constant namespace assembly on Linux x86-64

The namespace declaration `asm((expression));` accepts a constant-evaluated object
whose `size()` and `data()` supply one `.hidden SYMBOL` directive. The size counts
bytes and the pointer designates that many readable `char` objects. A final newline
is optional. The symbol is an ordinary x86-64 ELF definition or reference named by
its emitted spelling, including any asm label. [dcl.asm] leaves the interpretation
of the directive to the implementation.

Astra reviewed evaluating the receiver and accessor calls in one interpreter
invocation. The accessors use ordinary lookup, overload resolution and conversions
in the declaration's context. A private capture site reads the bytes while the
receiver is alive. The interpreter checks provenance, bounds, initialization and
its step budget and releases the bytes only if evaluation completes successfully.

Each item's evaluated directive follows its group into the completed translation
unit. The ELF assembler applies visibility to the existing symbol after ordinary
binding and section assignment. Relocation indices, definition metadata and weak
binding follow the existing rules.

[#974](https://github.com/yrashk/ycxx/issues/974) · [Statement asm #950](https://github.com/yrashk/ycxx/issues/950) · [Pinned runtime #468](https://github.com/yrashk/ycxx/issues/468)
