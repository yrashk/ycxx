# Namespace-scope reference bindings (#367)

C++ leaves storage for a reference unspecified ([dcl.ref]). The compiler
gives each ordinary namespace-scope definition a pointer-sized slot. An
`extern` declaration without an initializer declares its slot symbol. It
emits no storage. Lookup keeps the expression type and category of the
referent and records on the declaration reference that its symbol denotes a
binding slot. IR generation loads the pointer once when forming the glvalue.
Address-taking, reads, writes and further bindings use the resulting place.

Use the existing reference-binding checks ([dcl.init.ref]) before determining
how to initialize the slot. Constant evaluation returns the address of the
converted initializer's glvalue. This uses the built-in address operation,
independent of any overloaded `operator&`. Taking this address does not read
the referent. A successful binding to supported static storage is emitted as a
symbol and byte addend through the existing static pointer relocation path
([expr.const.init], [basic.start.static], #416). A constant-initialized
reference is usable in constant expressions. The interpreter presents its slot
as a pointer with that value and checks any subsequent access using the
referent's own constant-expression properties.

A binding that requires run-time evaluation uses the existing ordered
`StaticInit` path ([basic.start.dynamic]). Its slot has no data relocation
and the initializer stores the selected address once. The initialization
function has no referent destruction to register. Definition order supplies
the sequencing for later initializers in the same translation unit.

This slice covers scalar and array storage supported by the address evaluator
and dynamic binding expressions yielding existing storage. Class and base
subobject addresses that need additional interpreter layout remain under #25
and #367. Function reference declarations retain their existing semantic
handling. Runtime support for function references, namespace-scope lifetime
extension and guarded static references remain under #367. A binding involving
a materialized temporary requires lifetime support at object emission.
Inline and templated dynamic initialization remains under #368,
destruction on Linux under #400, and references requiring external ELF code
addresses under #414.

Decisions developed in Codex Astra's #367 design review and checked against
[dcl.ref], [dcl.init.ref], [expr.const.init], [basic.start.static] and
[basic.start.dynamic] in the public C++ draft.
