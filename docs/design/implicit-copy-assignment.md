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
