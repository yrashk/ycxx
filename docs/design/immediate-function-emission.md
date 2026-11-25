# Native collection of immediate functions

Immediate invocations evaluate at compile time. Nonvirtual immediate members and
function template specializations are candidates for native omission when their bodies
have neither closures nor static locals. Collection uses the concrete definition's
`DeclSema` flags. An instantiated definition carries its pattern's `consteval` flag
and a written explicit specialization carries its own.
[[expr.const.imm]](https://eel.is/c++draft/expr.const.imm) ·
[[temp.expl.spec]](https://eel.is/c++draft/temp.expl.spec)

Collection keeps the body and declaration through grouping and the existing front-end
ABI checks. `Program::function_ir` builds the evaluator's IR from the checked item or
instantiation. The object pipeline starts with the ordinary set of native definitions
except the omission candidates. It lowers those roots and follows calls, addresses
and the references in tables and data until every referenced candidate is included.
A surviving relocation from a retained immediate body therefore keeps the callee's
original native emission path. A definition used only for constant evaluation needs
no native lowering or symbol.

An ordinary virtual function may be referenced by a table independently of its
calls. An immediate virtual has no entry in the Itanium ABI table and is inline,
so it cannot be the key function. Class completion still includes it when matching
overrides, finding pure final overriders and deciding polymorphic layout. The
native table of an all-immediate class contains the header, with no function entries.
Matching overrides agree on immediacy. Virtual assignment remains outside this slice
because implicit assignments can introduce overrides.

The frontend checks a virtual immediate function's body and omits it from native
collection. The runtime slot used by the current call node cannot identify an
immediate virtual or its semantic final overrider. Until immediate virtual evaluation
is implemented, evaluated calls receive a diagnostic, including qualified calls.
Unevaluated operands can still use the selected declaration's type.
[[class.virtual]](https://eel.is/c++draft/class.virtual) ·
[[class.abstract]](https://eel.is/c++draft/class.abstract) ·
[Itanium C++ ABI §2.5.2](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vtable-components)

Nonvirtual immediate bodies with closures or static locals keep their existing
collection path, which provides their members or storage.
[#1097](https://github.com/yrashk/ycxx/issues/1097) ·
[#1077](https://github.com/yrashk/ycxx/issues/1077)
