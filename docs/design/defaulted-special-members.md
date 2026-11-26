# Special members defaulted on their first declaration

A default, copy or move constructor, a copy or move assignment operator or a nonvirtual destructor
whose first declaration, in its class, is `= default;` is user-declared and not user-provided. It
keeps its entity, access and type and stays in the overload set, suppresses the implicit members
its declaration suppresses and, for a constructor, prevents aggregate initialization. The item's
body is its definition. [[dcl.fct.def.default]](https://eel.is/c++draft/dcl.fct.def.default)

## Classification

`Program::special_members` classifies each one when its class is complete, or when its class
template specialization is instantiated. `classify_special_member` resolves the operation of each
base and member with ordinary overload resolution, from the context of the class and the function:
an lvalue or xvalue of the subobject's type initializes or assigns another, or it is
default-initialized. The class itself is still incomplete. The result is one of four states:

- **deleted**, where [class.default.ctor], [class.copy.ctor], [class.copy.assign] or [class.dtor]
  says so: an unusable, ambiguous or inaccessible subobject operation, a subobject destructor a
  constructor cannot invoke, a reference or const member, or a variant member with a nontrivial
  operation. A type that the implicit declaration cannot have also deletes the function: a
  volatile source, a const source of a move, a const source where the implicit one takes a
  mutable object, or a const-qualified assignment operator. A deleted move constructor or move
  assignment operator is not a candidate
  ([[over.match.funcs.general]](https://eel.is/c++draft/over.match.funcs.general));
- **trivial**: every subobject's operation is, and the class is not polymorphic. A volatile scalar
  member does not matter ([class.copy.ctor]/11). A volatile class member's operation is selected like
  any other, so it is deleted unless that class declares one that takes a volatile object; an
  implicit copy, move or copy assignment of a class with one is classified too. Uses lower to the
  representation copy, `DefaultInit` or `ValueInit`;
- **nontrivial**: uses call the function;
- **unsupported**: a subobject's operation met a limit of ycxx. A use is diagnosed, and native
  by-value boundaries are rejected.

An implicit copy constructor of a class with a subobject that has a defaulted copy is classified
the same way. The states give the class its call triviality (DECISIONS §1.6) and its POD layout.

A class template's own definition leaves its members unclassified. Only a specialization has
subobject types.

## Definitions

A nontrivial member, the destructor included, is implicitly defined where something uses it.
`required_members` finds the use and synthesizes the body: the bases, then the members, each from
the corresponding subobject, direct-initialized, so a constructor template taking anything does not
see the derived object. An array is initialized or assigned element by element, or as a whole when
the operation overload resolution selects for its elements is trivial. Analysis of a specialization's definition is
at the end of the unit, since its arguments may be declared after the template. Definitions are
`constexpr`, and weak unless the class is local to the unit.

A default constructor's definition is the one a written constructor with an empty body would have.
Value-initialization zero-initializes first.

## Exception specifications

A written `noexcept` is the specification. Without one, `defaulted_nonthrowing` decides it where a
`noexcept` expression asks, from the definition's expressions ([except.spec]). A constructor's does
not include its subobjects' destructors. The function's type is not changed.

A trivial member whose specification is potentially-throwing (a written `noexcept(false)`, or a
definition that uses such a member) is called instead of lowered (`MemberDeclRec::trivial_call`),
so that `noexcept` sees it. It stays trivial for the ABI.

## Implicit moves

ycxx uses an implicit copy where an implicit move would move a subobject by a constructor of its own
([#244](https://github.com/yrashk/ycxx/issues/244)). A defaulted move over such a subobject, and an
implicit move of a class with a defaulted copy, are unsupported, not copies.

## Layout

The Itanium ABI's POD for layout, whose tail padding is not reused, depends on the `-fcompat` mode
(docs/compatibility.toml). In Clang mode on arm64 macOS it is C++11's: trivial and
standard-layout, where a deleted default constructor counts as trivial, and no class with a
volatile class member is. Elsewhere, in spec and GCC modes and in Clang mode on x86-64 Linux, it is
C++03's: no user-provided constructor, destructor or copy assignment, only public members and no
bases. Defaulted and deleted special members are not user-provided in either. GCC 16 agrees on Linux except for class template
specializations, whose defaulted constructors it counts as user-provided.

Defaulting outside the class, local classes, virtual assignment operators and a requires-clause are
in [#157](https://github.com/yrashk/ycxx/issues/157). Implicit moves and assignment are in
[#244](https://github.com/yrashk/ycxx/issues/244) and [#360](https://github.com/yrashk/ycxx/issues/360).
