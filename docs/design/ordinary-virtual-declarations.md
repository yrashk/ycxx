# Ordinary virtual declarations (#309)

The first slice analyses ordinary root classes with initial in-class declarations of
implicit-object virtual member functions. It checks their inline bodies in the usual
complete-class context. A virtual destructor may be declared, defaulted on its first
declaration, or defined inside the class, including with `constexpr`.
[[dcl.fct.spec]](https://eel.is/c++draft/dcl.fct.spec) ·
[[class.virtual]](https://eel.is/c++draft/class.virtual) ·
[[class.dtor]](https://eel.is/c++draft/class.dtor) ·
[Astra's review](https://github.com/yrashk/ycxx/issues/309#issuecomment-7054637433)

The declaration holds the virtual property. The class record derives polymorphism and
virtual destruction from its members. An ordinary virtual function makes the class
non-aggregate, nonempty and nontrivially copyable. A virtual destructor is nontrivial
even when it is defaulted. The existing class trait builtins can read these facts without
asking for layout. Pointers and references retain their usual type and representation.
The type can also appear by value in a function declaration.
[[class.copy.ctor]](https://eel.is/c++draft/class.copy.ctor) ·
[[class.copy.assign]](https://eel.is/c++draft/class.copy.assign) ·
[[meta.unary.prop]](https://eel.is/c++draft/meta.unary.prop)

The class record carries whether its layout needs virtual support. The property extends
through value members, array elements and base subobjects and reaches the cached native
shape. Syntax-only checks diagnose layout queries, materialization, object definitions,
evaluated calls and by-value function definitions that need that layout. Virtual calls and
external virtual definitions also receive an explicit diagnostic. A translation unit with
a virtual declaration can be checked with `-fsyntax-only`. A native request reports the
first such declaration before emitting the unit. The back end also declines a cached
class shape requiring virtual layout.

Derived virtual members, pure virtual functions, explicit virtual destructor calls and
virtual member addresses are tracked in [#309](https://github.com/yrashk/ycxx/issues/309).
That issue also tracks peer dispatch and native table emission. Virtual destructor variants
are tracked in [#136](https://github.com/yrashk/ycxx/issues/136), RTTI operations in
[#294](https://github.com/yrashk/ycxx/issues/294), virtual bases in
[#310](https://github.com/yrashk/ycxx/issues/310), and further exception specifications in
[#219](https://github.com/yrashk/ycxx/issues/219).
