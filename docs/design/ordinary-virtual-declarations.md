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

An ordinary named namespace-scope class or struct can place `final` after its class name
and before the base clause or body. The grammar still uses `final` as an identifier in
class names and object declarators. A repeated specifier receives a diagnostic at the
second token.

The defining declaration supplies the property to the completed class record. Base-clause
validation reads the complete class resolved by the base type, including through an alias.
A bare redeclaration leaves the completed property in effect. Class identity and layout use
the same rules as for a definition without the property.
[[class.pre]](https://eel.is/c++draft/class.pre) · [#309](https://github.com/yrashk/ycxx/issues/309)

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
either prevents admission to the ordinary root or primary-base layout profiles. The
borrowed-interface call profile below uses the address point in an existing object.
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
passing its address. Value initialization, copying, arrays and by-value transfer remain guarded.
[[dcl.init]](https://eel.is/c++draft/dcl.init) · [#309](https://github.com/yrashk/ycxx/issues/309)

The maker loads `_ZTV` through the GOT. A split maker has an undefined table reference. In a
translation unit that also owns the table, the GOT relocation names the same ELF symbol entry
that defines the table. Ownership follows the key definition across the complete unit even if the
maker appears first.
[#309](https://github.com/yrashk/ycxx/issues/309)

## Linux x86-64 borrowed abstract interfaces

An ordinary named, externally linked class with no bases or fields can supply one or two
public pure slots. The admitted signatures spell `int` or `unsigned int` directly, have
at most one integer argument, allow member `const` and require bare `noexcept`. The
same dispatch check applies at the call site and during translation-unit emission,
which checks the completed declarations and available ABI information.

The caller uses an unqualified written object call on a pointer or lvalue reference.
Lowering reads the address point from the supplied interface subobject, indexes in
declaration order and passes the same subobject pointer to the selected function. A
peer's secondary-base thunk can adjust it to the complete object. The consumer's
ELF object contains the ordinary peer helper references and the indirect call. A bare
`noexcept` slot retains its nonthrowing prototype through conditional initialization.

Mixed GCC and Clang fixtures cover slot order, one receiver and argument evaluation,
and native secondary-base adjustment. Resident edits swap declarations and change
slot eligibility, comparing the resulting objects or diagnostics with cold compilation.
[Itanium call ABI](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vcall.caller) ·
[#309](https://github.com/yrashk/ycxx/issues/309) · [macOS parity #659](https://github.com/yrashk/ycxx/issues/659)

## Linux automatic construction with one abstract primary base

A plain automatic declaration can construct an ordinary concrete class with one direct
public nonvirtual base. The base declares one public pure virtual `int f() = 0` and has
no fields. The derived class declares one public `int` field without an initializer and
one matching public `int f() override`. Both classes have implicit default constructors and
trivial nonvirtual destructors. Complete declarations establish ordinary external visibility
and alignment for both classes.

The certified primary base contains one vptr and occupies offset zero. The derived field
is at offset eight on Linux x86-64, and the derived object has size 16 and alignment 8.
The base constructor is nontrivial because the class is polymorphic. This shape has no
base constructor body, field initialization or other action that observes the intermediate
base address point. The existing automatic initializer stores the derived `_ZTV + 16`
before any user statement. The scalar field follows ordinary default initialization.

The class fact supplies the layout of this base edge and the storage for the marked
automatic object. The maker can access that object's field and bind a base lvalue
reference to it. A maker-only unit has an undefined `_ZTV` reference. When the key definition
is in the same unit, the GOT relocation names the defined symbol at the same index. Borrowed
dispatch continues to use its own declaration check.
[[class.cdtor]](https://eel.is/c++draft/class.cdtor) ·
[Itanium ABI §2.4](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#class-types) ·
[#309](https://github.com/yrashk/ycxx/issues/309)

## Linux x86-64 key-owned derived table

The unit with the directly written out-of-line, non-inline `int D::f()` definition owns the
table for the automatic derived shape. The complete unit establishes ownership independently
of construction or source order. Both classes, the pure declaration and the key must use the
default-visible ABI. The producer checks later declarations, attributes and pragmas before
emitting an object. A definition of the pure base slot is outside the profile.

The global default-visible 24-byte `_ZTV` contains offset-to-top zero, the derived `_ZTI`
pointer and one function pointer to `D::f`. The 24-byte derived `_ZTI` contains the runtime
`__si_class_type_info` address point, its `_ZTS` pointer and the base `_ZTI` pointer. The base
has no key function. Its weak 16-byte descriptor contains the runtime `__class_type_info`
address point and its `_ZTS` pointer, with `.data.rel.ro` and its ABS64 RELA section in an ELF
COMDAT keyed by the base `_ZTI`. Each `_ZTS` is a separate weak read-only COMDAT holding the
Itanium type encoding followed by NUL. The table needs neither a base vtable nor a pure-virtual
handler.
[Itanium ABI §2.5.2](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vtable-components) ·
[Itanium ABI §2.9.5](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#rtti-layout) ·
[#309](https://github.com/yrashk/ycxx/issues/309) · [#422](https://github.com/yrashk/ycxx/issues/422) ·
[macOS parity #659](https://github.com/yrashk/ycxx/issues/659)

## General single-inheritance dynamic classes (both targets)

A polymorphic class outside the peer profiles above is a *dynamic class* when it is an ordinary
non-template class with external linkage, at namespace scope or nested in such classes, and has
at most one direct base, which is public, nonvirtual and itself dynamic. Class completion records
its primary table (`sema::DynamicClass`): the base's slots, each overridden one replaced by its
overrider, then the virtuals that override nothing in declaration order. A virtual destructor
occupies a complete and a deleting slot, and an implicit destructor replaces both inherited ones.
The key function is the first virtual declared in the class that is neither pure nor inline at
the end of its definition. The profiles keep their classes; a profile class is not a dynamic base.
[Itanium ABI §2.5.2](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vtable-components) ·
[Itanium ABI §5.2.3](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vague-vtable)

A dynamic root has its vptr at offset zero; a derived class shares its primary base's. A class is
or contains a polymorphic class unsupported by this layout when `virtual_layout_unsupported` says
so, and only those keep the layout guard. A dynamic class without user-declared constructors gets
an implicitly defined default constructor whose body is the one a user-written constructor with no
ctor-initializer and an empty compound statement would have ([class.default.ctor]), with its
default member initializers. Value-initialization zero-initializes first. A dynamic class, or a
class containing one, gets an implicit copy constructor that copies a dynamic base with the base's
copy constructor. A virtual destructor defaulted on its first declaration gets the implicit body.
[[class.default.ctor]](https://eel.is/c++draft/class.default.ctor) ·
[[class.copy.ctor]](https://eel.is/c++draft/class.copy.ctor)

Every constructor of a dynamic class stores its table's address point after the base subobject is
initialized and before the first member; every destructor stores it before its body, so virtual
calls during construction and destruction use the class whose constructor or destructor runs.
Their base-object entries share the complete-object code (no virtual bases). A virtual
destructor's definition adds the deleting entry, which runs the complete entry and then calls
sized `operator delete` with the class's size.
[[class.cdtor]](https://eel.is/c++draft/class.cdtor) ·
[Itanium ABI §5.1.4.3](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#mangling-special-ctor-dtor)

An unqualified call of a virtual member through any object expression loads the vptr, indexes the
slot the declaration has in its own class's table, and passes the object pointer. A qualified
name calls the function directly ([class.virtual]/16).
[Itanium ABI §3.2.4](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vcall.caller)

A unit defining the key function out of class, not inline, owns a strong table and RTTI. A class
without a key function (or with an inline one) gets a weak table where a constructor or
destructor emitted in the unit installs it, as Clang emits such a table where it is used. The
implicit constructors of dynamic classes are emitted where something calls them. A derived
descriptor names its base's; a keyless base whose table is not used in the unit gets its
descriptor alone. A destructor destroys a base defined elsewhere with the base's D2 entry. A
root's descriptor is a
`__class_type_info`; a derived class's is an `__si_class_type_info` that names its base's,
referenced by name or defined in the same object. Pure slots hold `__cxa_pure_virtual`. On ELF a
weak table and descriptor each have a COMDAT group keyed by its name; on Mach-O they are weak
definitions, with weak descriptors and names following Apple's non-unique RTTI convention as the
keyless descriptor above does. The run suite (`tests/own/classes/virtual-dispatch`) runs on macOS,
and `unit.linux_virtual_dispatch` links the same tests with native GCC and Clang on Linux,
including the mixed-object peers.

A field declared through `decltype` keeps the class out (sema can lose an alias's alignment
there, #1167), as does a class in an unnamed namespace (#1155).

A class template specialization is a dynamic class by the same rules (#1153). Its template's
definition is never laid out or abstract, and checks no override that could depend on an argument;
each specialization checks its members against its substituted base. An implicit specialization
has no key function: every unit that installs its table emits the table, its RTTI and the virtual
members in its slots weakly, instantiating those members there ([temp.inst] leaves it unspecified
whether unused ones are). Synthesized special members of every dynamic class, the implicit
destructor included, are emitted only where something uses them, so a table that nothing installs
instantiates nothing. A specialization for a TU-local type needs a local table (#1155).
[[temp.inst]](https://eel.is/c++draft/temp.inst)

Local and internal-linkage classes, other base shapes (non-polymorphic or
non-public bases, multiple inheritance), `delete` through a virtual destructor, covariant returns,
`typeid`, and constant evaluation of dynamic objects remain open; their issues are listed in #309.

Broader table emission, covariant returns, further pure-slot signatures, out-of-class pure member
definitions, explicit virtual destructor calls and virtual member addresses
remain in #309. Pure declarations and `final` for virtual destructors and assignment operators
also remain there. A virtual assignment operator can be overridden by an implicitly declared
copy or move assignment in a derived class.
Separate issues track virtual destructor entry points, RTTI operations, virtual bases and
further exception specifications.
[#309](https://github.com/yrashk/ycxx/issues/309) · [#136](https://github.com/yrashk/ycxx/issues/136) ·
[#294](https://github.com/yrashk/ycxx/issues/294) · [#310](https://github.com/yrashk/ycxx/issues/310) ·
[#219](https://github.com/yrashk/ycxx/issues/219)
