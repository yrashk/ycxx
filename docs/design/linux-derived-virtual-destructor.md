# A derived virtual destructor with a peer-owned primary base

The pinned libycxx configuration probe defines a destructor for a named, ordinary
class `D` with one public nonvirtual base `B`. On Linux x86-64 the certified `B`
has a single public virtual destructor and one naturally aligned ordinary pointer
field. Its destructor is the key and belongs to a peer. `D` adds no data members
and its public overriding destructor is its only written member. Its empty out-of-class
definition is the key. Both destructors must have a nonthrowing specification.
An implicit nonthrowing destructor specification is sufficient.

Astra reviewed the layout and ownership against the Itanium ABI. The base contains
an eight-byte vptr and the pointer field. It is also the primary base at offset zero
of the derived object. `D1` and `D2` share a body that sets the primary vptr to
the address point of `D`'s table and calls the peer's `B` base-object destructor
once. `D0` calls `D1` and then the ordinary global sized deallocator with the
original pointer and the complete object's computed size.

The derived 32-byte table has zero offset-to-top and two destructor slots in
complete-object then deleting order. Its single-inheritance RTTI descriptor imports
the named descriptor for `B`. Only `D` owns a table or RTTI in this translation unit.
The producer checks the complete unit for redeclarations, visibility and layout
directives. These facts certify this destructor's lowering and table only. The existing
automatic-object, virtual-call and typed-cast layout profiles keep their own rules.
When both destructors are only declared, their definitions, tables and RTTI belong
to the peer units that define the keys.

[#999](https://github.com/yrashk/ycxx/issues/999) · [Destructor variants #136](https://github.com/yrashk/ycxx/issues/136) · [Virtual classes #309](https://github.com/yrashk/ycxx/issues/309) · [RTTI #294](https://github.com/yrashk/ycxx/issues/294) · [Itanium ABI §2.5.2](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vtable-components)
