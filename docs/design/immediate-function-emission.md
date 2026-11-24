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

Virtual functions keep their native path because a table slot can name one
independently of its calls. A body with closures or static locals also keeps
its existing collection path, which provides their members or storage.
[#1097](https://github.com/yrashk/ycxx/issues/1097) ·
[#1077](https://github.com/yrashk/ycxx/issues/1077)
