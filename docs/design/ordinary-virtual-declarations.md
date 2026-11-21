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

## One ordinary polymorphic base

A non-template namespace-scope class may derive from one complete ordinary polymorphic
class through a nonvirtual base specifier. Override matching follows that single base chain
independently of name lookup and access. For each function, the nearest virtual declaration
with matching adjusted parameter types, variadic status, cv-qualifiers and ref-qualifier
supplies the inherited signature. Ordinary lookup still hides base overloads. An overriding
declaration acquires virtuality even without a virt-specifier. Return types must agree and
the override must preserve a nonthrowing exception specification. Ref-qualified virtual
declarations retain their qualifiers. Calls involving those overload sets still require
ref-qualified overload resolution.
[[class.virtual]](https://eel.is/c++draft/class.virtual) ·
[[class.member.lookup]](https://eel.is/c++draft/class.member.lookup) ·
[[except.spec]](https://eel.is/c++draft/except.spec) ·
[Astra's review](https://github.com/yrashk/ycxx/issues/309#issuecomment-7056328877)

A derived destructor, including an implicit one, is virtual when the base destructor is
virtual. At class completion an omitted exception specification depends on the effective
specifications of base and field destructors. An override requires a specification the
compiler can establish. Constructor bodies can check the implicit default initialization
of the direct polymorphic base with the existing constructor overload and access checks.
An unqualified name that resolves to that direct base can also designate it in a
parenthesized or flat braced initializer. Overload resolution selects the constructor,
and the derived constructor's context determines access to the selected constructor
and the potentially invoked base destructor. The semantic initializer retains the
selected constructor's arguments for deferred expression checks. Braces apply ordinary
list narrowing. Constructor overload sets that need the initializer-list constructor
phase remain in [#329](https://github.com/yrashk/ycxx/issues/329).
[[class.dtor]](https://eel.is/c++draft/class.dtor) ·
[[class.base.init]](https://eel.is/c++draft/class.base.init) ·
[[over.match.list]](https://eel.is/c++draft/over.match.list)

The virtual layout guard applies to evaluated construction, constant evaluation of
polymorphic objects, and native emission of their constructors. Class templates continue
to reject polymorphic bases. Implicit polymorphic copy construction remains in
[#244](https://github.com/yrashk/ycxx/issues/244). Immediate function references outside the
supported overload-set checks are tracked in [#102](https://github.com/yrashk/ycxx/issues/102).

## Linux x86-64 access to external objects

An ordinary root class can declare virtual functions for calls on objects supplied by an
external translation unit. Its declarations have fixed integer
signatures and it leaves construction, destruction and all virtual definitions to the
provider. Semantic analysis records the selected virtual declaration's slot in declaration
order. The caller evaluates the receiver once, loads its virtual table address point and
the slot, and passes the same receiver pointer as `this` to the indirect call. An explicit
qualification with the receiver's own class name selects a direct call of that declaration.
[[expr.call]](https://eel.is/c++draft/expr.call) ·
[Itanium ABI §3.2.4](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vcall.caller) ·
[Astra's review](https://github.com/yrashk/ycxx/issues/309#issuecomment-7057422652)

A complete declaration-only root with natural layout can expose directly written builtin
integral fields of up to 64 bits. The shared record layout reserves one target-sized, aligned
vptr at offset zero and places source fields after it in declaration order. Sema uses the layout
for `sizeof` and `alignof`. Native field projections use the same offsets with the existing
access and qualification rules. Both also support the fieldless root, whose size and alignment
come from the vptr. [Itanium ABI §2.4](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#class-types)

An ordinary derived class can expose a single public nonvirtual base that is such a root.
It can declare one directly written integral field and one noncovariant override with a fixed
integer signature. The shared layout places the root at offset zero and aligns the derived
field after the root's nonvirtual data. A pointer upcast preserves the address or null value,
and a reference upcast designates the base subobject. The override's validated base
declaration determines the slot, so calls through either view dispatch through the same
virtual table address point. [Itanium ABI §2.4](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#class-types) ·
[Itanium ABI §2.5.2](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vtable-components) ·
[[conv.ptr]](https://eel.is/c++draft/conv.ptr)

The native-unit check admits these consumer declarations and checks the unit for virtual
definitions. The layout applies to field accesses on objects the provider has already
constructed. Whole-object storage, construction, destruction, copying, and by-value transfer
remain guarded. Packed or annotated layout, further inheritance forms, and virtual table
production are tracked in [#309](https://github.com/yrashk/ycxx/issues/309) and [#489](https://github.com/yrashk/ycxx/issues/489).
Class descriptors and RTTI are tracked in [#294](https://github.com/yrashk/ycxx/issues/294),
destructor entry points in [#136](https://github.com/yrashk/ycxx/issues/136), and group emission
in [#422](https://github.com/yrashk/ycxx/issues/422).

Covariant return types, pure virtual functions, `final`, explicit virtual destructor calls,
virtual member addresses, and native table emission are tracked in
[#309](https://github.com/yrashk/ycxx/issues/309). Virtual destructor variants
are tracked in [#136](https://github.com/yrashk/ycxx/issues/136), RTTI operations in
[#294](https://github.com/yrashk/ycxx/issues/294), virtual bases in
[#310](https://github.com/yrashk/ycxx/issues/310), and further exception specifications in
[#219](https://github.com/yrashk/ycxx/issues/219).
