# Implicit copy assignment of class members

An implicitly declared copy assignment participates in member overload resolution. Its
parameter is a reference to const when each class subobject has a copy assignment that
accepts const. Availability of the selected operation is a separate question. A user-declared
copy assignment suppresses this declaration. An otherwise implicit copy becomes deleted
after a user-declared move constructor or move assignment. [#360](https://github.com/yrashk/ycxx/issues/360)

For ordinary, non-template classes with no bases, check and build the selected implicit copy
on demand in its class's access context. Assign each scalar or class member in declaration
order, using normal member overload resolution for class fields. An unusable subobject
assignment deletes the implicit operation. Return the original destination as an lvalue.
A call on a member can observe source and destination aliasing, including self-assignment.
[Pinned `[class.copy.assign]`](https://github.com/cplusplus/draft/blob/65890fab817ca1dfe7898f481c4fd088bbe2bbce/source/classes.tex#L1707)

Assignment notation evaluates the right operand before the left. Ordinary explicit member
call notation follows ordinary call sequencing. Generate code for synthesized bodies that
are required by a use and give them ordinary inline linkage. Use the existing representation
copy only when the selected implicit operation is proven trivial and the subobject layout
and qualifiers permit that copy. Braced assignment has the same proof requirement.
[`[expr.assign]`](https://eel.is/c++draft/expr.assign)

Astra reviewed the declaration, selection, availability, sequencing and lowering rules.
Implicit move and nontrivial base, union and array assignment remain in
[#360](https://github.com/yrashk/ycxx/issues/360). The surrounding declaration and
inheritance work is tracked by [#244](https://github.com/yrashk/ycxx/issues/244) and
[#308](https://github.com/yrashk/ycxx/issues/308).

## Effects of user-declared assignment

Classify a non-template assignment member by its parameter. The class by value or
by lvalue reference declares copy assignment. The class by rvalue reference declares
move assignment. Use the canonical parameter type so aliases and its cv-qualifiers
receive the same classification.
[`[class.copy.assign]`](https://eel.is/c++draft/class.copy.assign)

A user-declared move assignment deletes an otherwise implicit copy constructor and
suppresses the implicit move constructor. A user-declared copy assignment also
suppresses the implicit move constructor while leaving the copy constructor to the
ordinary declaration rules. Apply the resulting constructor and destructor facts
before choosing a calling convention.
Astra reviewed the declaration and call-triviality rules.
[`[class.copy.ctor]`](https://eel.is/c++draft/class.copy.ctor) ·
[Itanium C++ ABI](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#non-trivial) ·
[#244](https://github.com/yrashk/ycxx/issues/244)

## First-declaration defaulted copy assignment

An ordinary namespace-scope class can define `C& operator=(C&)` or
`C& operator=(const C&)` as `= default` on its first member declaration.
Keep its written declaration, entity, access and parameter type. The definition
is implicitly inline and constexpr. Preserve a written exception specification.
The declaration counts as user-declared when determining implicit special members.
An ordinary in-class deleted copy assignment also counts and participates in
overload resolution as a deleted function. Both forms are non-user-provided on
their first declaration, so either retains the base layout of an otherwise
equivalent class with implicit assignment.
[[dcl.fct.def.default]](https://eel.is/c++draft/dcl.fct.def.default) ·
[[class.copy.assign]](https://eel.is/c++draft/class.copy.assign)

Namespace-scope class templates also accept a deleted copy assignment on its
first in-class declaration. Instantiating the class replays the member declaration
with the specialization's type. The existing deleted flag remains available
to member overload resolution and implicit special-member classification.
[[temp.inst]](https://eel.is/c++draft/temp.inst) ·
[[dcl.fct.def.delete]](https://eel.is/c++draft/dcl.fct.def.delete) ·
[#709](https://github.com/yrashk/ycxx/issues/709)

Share the implicit assignment classifier for subobject signatures. A written
const source parameter with an otherwise implicit mutable source parameter
defines the operation as deleted. Reference members and const non-class members
also cause deletion. For ordinary classes with supported fields, use the same
on-demand memberwise body and access context as implicit assignment. Check the
written member's access after selection. The body assigns each field in declaration
order and returns the destination by reference. Unusable field assignments cause
deletion, and operations outside the supported lowering receive an unsupported
diagnostic when selected. Only the implicit assignment path uses its existing
representation-copy optimization.
[[class.copy.assign]](https://eel.is/c++draft/class.copy.assign)

Explicit move, defaulting outside the class, and additional base, array and union
operations remain in [#244](https://github.com/yrashk/ycxx/issues/244) and
[#360](https://github.com/yrashk/ycxx/issues/360). Related work covers virtual
members in [#309](https://github.com/yrashk/ycxx/issues/309), defaulting constructors
and destructors in [#157](https://github.com/yrashk/ycxx/issues/157), inferred
exception specifications and `noexcept` expressions in [#219](https://github.com/yrashk/ycxx/issues/219),
and class constant evaluation in [#25](https://github.com/yrashk/ycxx/issues/25).
