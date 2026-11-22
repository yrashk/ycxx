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
shape. The compiler diagnoses layout queries, materialization, object definitions, evaluated
calls, by-value function definitions and external virtual definitions when they need support
beyond the cases below. The native unit check reports the first unsupported virtual
declaration before object emission. The back end also checks cached class shapes.

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

`final` belongs to the individual ordinary virtual member declaration. It can appear with
`override` in either order. Both specifiers are contextual identifiers and may each appear
once after the declarator. A later declaration that overrides a member marked `final` is
ill-formed. The same base-chain match covers implicit overrides of private members and of
declarations hidden by unrelated overloads.
[[class.virtual]](https://eel.is/c++draft/class.virtual) ·
[[class.mem.general]](https://eel.is/c++draft/class.mem.general) ·
[Scope review](https://github.com/yrashk/ycxx/issues/309#issuecomment-7079653899)

An ordinary non-template virtual member may have a pure-specifier with the exact token `0`
after `=` and preprocessing. The pure-specifier follows any virt-specifiers. At class
completion the derived class starts with its base's
pure final overriders. An override removes the inherited member from that sequence and adds
its own declaration when it is pure. A declaration alone can make an inherited slot concrete.
`__is_abstract` tests whether the completed class still has any pure final overrider.
[[class.abstract]](https://eel.is/c++draft/class.abstract) ·
[[class.mem.general]](https://eel.is/c++draft/class.mem.general) ·
[Scope review](https://github.com/yrashk/ycxx/issues/309#issuecomment-7080764178)

Abstract types can name base subobjects, pointer and reference targets, nondefining
namespace-scope extern declarations and by-value function declarations. Object and field
definitions, allocation and by-value calls and function definitions check the abstract
type. A direct `decltype(make())` can name an abstract result without materializing it.
`sizeof(make())` and member access on `make()` require materialization. The Linux virtual
layout admission checks each declaration in the root and derived class. A pure member in
either keeps the hierarchy outside native emission even when a concrete override exists.
[[basic.def]](https://eel.is/c++draft/basic.def) · [[dcl.type.decltype]](https://eel.is/c++draft/dcl.type.decltype) ·
[[conv.rval]](https://eel.is/c++draft/conv.rval) · [#309](https://github.com/yrashk/ycxx/issues/309)

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
external translation unit. Its declarations have fixed integer signatures. Semantic
analysis records the selected virtual declaration's slot in declaration
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
definitions. A unit can also define one later virtual member out of class, with a
directly written integer signature and a matching exception specification. The declaration
and definition use an omitted specification, bare `noexcept`, or a `true` or `false` literal
condition. The first eligible declaration is the Itanium key function. The peer defines
the key and provides the virtual table and RTTI. The supported member uses the normal
implicit receiver and direct field offsets. [Itanium ABI §5.2.3](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vague-vtable)

The layout applies to field accesses on objects the provider has already constructed.
The separate automatic root case below also uses that layout. Copying and by-value transfer
remain guarded. Packed or annotated layout and further inheritance forms are tracked in
[#309](https://github.com/yrashk/ycxx/issues/309) and [#489](https://github.com/yrashk/ycxx/issues/489).

## Linux x86-64 key-owned root tables

The producer admits one ordinary, externally linked, default-visible non-template root
at namespace scope. Its natural layout has one directly declared public integral
field. It declares two virtuals with directly written fixed integer signatures. The first
declaration is the Itanium key function. The unit defines it out of class and non-inline in a
compound body with a matching signature and exception specification. The peer defines the other
member. ABI-affecting attributes and a GNU visibility pragma
preceding the key definition are outside this scope.
[Astra's review](https://github.com/yrashk/ycxx/issues/309#issuecomment-7077374439)

The complete translation unit determines ownership independently of table use. An immutable
unit value records the nominal class type and the two function entities
in declaration order. Each function's source projection continues to determine its lowering
independently. The resident tests swap the declarations and verify that ownership moves
between units and dispatch follows the new slot order. A body-only edit preserves the table
metadata and unrelated object bytes. Cold and daemon builds produce identical objects, and
mixed GCC and Clang objects verify dispatch and RTTI.

The ELF `_ZTV` starts at section offset zero and occupies 32 bytes. It starts with a zero
offset-to-top, then a pointer to `_ZTI` and the two function pointers in declaration order.
The 16-byte `_ZTI` is an Itanium root class descriptor. It points to the runtime
`__class_type_info` virtual table address point at a byte offset of 16 and to the NUL-terminated `_ZTS`
object. The type encoding shared by all three names is the mangler's standalone type encoding.
`_ZTV` and `_ZTI` are global default-visible definitions in `.data.rel.ro`. `_ZTS` is a sized,
weak default-visible object in a separate read-only COMDAT keyed by its own name.
[Itanium ABI §2.5.2](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vtable-components) ·
[Itanium ABI §2.9.5](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#rtti-layout) ·
[Itanium ABI §5.2.3–4](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vague-vtable) ·
[ELF group output #422](https://github.com/yrashk/ycxx/issues/422)

## Linux x86-64 automatic root initialization

Linux x86-64 supports a plain declaration such as `Root root;` in an ordinary
non-template free function. The supported root has an implicit default constructor, a trivial
destructor and one public integral field without a default member initializer. The declaration
names the root directly and uses its natural alignment. Lowering allocates the object, installs
the address point `_ZTV + 16` in its vptr at offset zero and preserves ordinary default
initialization of the integral field. The local supports direct integral member access and
passing its address. Value initialization, copying, arrays, by-value transfer and construction
of derived objects remain guarded.
[[dcl.init]](https://eel.is/c++draft/dcl.init) · [#309](https://github.com/yrashk/ycxx/issues/309)

The maker loads `_ZTV` through the GOT. A split maker has an undefined table reference. In a
translation unit that also owns the table, the GOT relocation names the same ELF symbol entry
that defines the table. Ownership follows the key definition across the complete unit even if the
maker appears first.
[#309](https://github.com/yrashk/ycxx/issues/309)

Broader table emission, covariant returns, pure-slot dispatch, out-of-class pure member
definitions, class-head `final`, explicit virtual destructor calls and virtual member addresses
remain in #309. Pure declarations and `final` for virtual destructors and assignment operators
also remain there. A virtual assignment operator can be overridden by an implicitly declared
copy or move assignment in a derived class.
Separate issues track virtual destructor entry points, RTTI operations, virtual bases and
further exception specifications.
[#309](https://github.com/yrashk/ycxx/issues/309) · [#136](https://github.com/yrashk/ycxx/issues/136) ·
[#294](https://github.com/yrashk/ycxx/issues/294) · [#310](https://github.com/yrashk/ycxx/issues/310) ·
[#219](https://github.com/yrashk/ycxx/issues/219)
