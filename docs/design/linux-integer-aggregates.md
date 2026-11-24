# Linux AMD64 integer aggregate calls (#440)

The first slice covers fixed-prototype direct calls to ordinary free functions. By-value
parameters and results can have a complete, nonempty, flat record layout of 1–16 bytes,
with alignment up to eight. The record must be trivial for calls under DECISIONS §1.6.
Its non-static data members have naturally aligned, non-overlapping integral scalar,
ordinary object-pointer or object lvalue-reference layouts. The signature check rejects records with bases,
bit-fields, unions, nested classes or arrays, and fields requiring other ABI classes.
Member functions, closure call operators, function template specializations and calls
through a pointer to a fixed-prototype function use the same placement, with `this` as the
first INTEGER argument (#640). Calls with an ellipsis stay outside this slice.

Results in memory and classes not trivial for calls (#641). A class result of more than two
eightbytes, or of a class not trivial for calls, is returned through a hidden pointer. `lower`
makes that pointer the first L2 argument on both sides, so it takes RDI ahead of `this`, and
the emitter returns it in RAX (AMD64 psABI §3.2.3; Itanium C++ ABI §3.1.3). A class argument
not trivial for calls passes by the address of the caller's object, as on AArch64 (Itanium
§3.1.2.3). A trivially copyable argument of more than two eightbytes is passed by value on the
stack (MEMORY), which is not implemented yet.

An ordinary empty, naturally laid out one-byte class trivial for calls has only NO_CLASS in a
fixed direct argument (AMD64 psABI §3.2.3, Itanium C++ ABI §3.1.2.6). L1 evaluates and
materializes the argument as usual. L2 omits its transport before assigning registers and stack
slots. The callee gives each such parameter separate one-byte storage. A constructor's `this`
and a following owner reference therefore occupy the first two INTEGER registers. The empty
result convention is tracked separately in [#641](https://github.com/yrashk/ycxx/issues/641).

The front end records when a layout attribute or active packing directive makes the
computed natural layout uncertain. The by-value ABI check rejects those shapes even
if their computed offsets fit, since a peer can apply the layout directive. This check
uses the pragma state at the definition, including simple push/pop and explicit reset.
For an instantiated record it reads the source of the selected template pattern.
Actual packed layout remains tracked in [#307](https://github.com/yrashk/ycxx/issues/307).

When analysis runs in groups, Astra recommends carrying layout uncertainty by entity ID
from each group, including declarations that do not contribute a class shape. The unit
combines the uncertainty from current groups before checking the retained shapes. A
nonvirtual constructor or destructor's base-object variant also records the entities whose
layouts its existing eligibility check relies on. The final unit keeps the variant when none
of those layouts are uncertain. [#992](https://github.com/yrashk/ycxx/issues/992)

Astra checked the scope and calling convention against the public
[System V AMD64 psABI](https://gitlab.com/x86-psABIs/x86-64-ABI) §§3.2.2–3.2.3,
the [Itanium C++ ABI](https://itanium-cxx-abi.github.io/cxx-abi/abi.html)
§§3.1.2.3 and 3.1.3 and the pinned [expr.call], [class.copy.ctor] and
[class.temporary]. Each laid-out field contributes INTEGER to the eightbytes its
bytes occupy. Padding contributes NO_CLASS. Preserve the real little-endian byte
offsets and bound each load or store by the object size. INTEGER results use RAX
and RDX in eightbyte order.

Astra checked the object-pointer extension against AMD64 psABI §§3.1.2 and 3.2.3.
An immediate `TPointer` whose pointee is an object type or cv-qualified `void`
contributes INTEGER using the target's pointer size and alignment. This includes
pointers to incomplete classes and arrays. Astra also checked the object lvalue-reference
extension in [#988](https://github.com/yrashk/ycxx/issues/988). Each such field, including a
reference capture, contributes INTEGER using the size and alignment of its stored address.
Copying the parameter's storage preserves the field's binding to the original referent.
Fields of function-pointer, function-reference, rvalue-reference and pointer-to-member type
remain [#440](https://github.com/yrashk/ycxx/issues/440) work.

The signature check and lowering take the classification from the same unit-local
class layout. The direct callee check keeps using the definition's deduced function
type when the declaration still has a placeholder (#441). L1 keeps its by-value
argument objects and class-result destination. L2 lowering packs the objects into
integer operands and results and reconstructs them in bounded storage. The function
entry makes a separate parameter object, so writes through an explicit alias to
the original object remain independent of the parameter.

The entry and call lowering share AMD64 argument placement. Walk logical arguments
left to right and reserve all of a record's INTEGER registers together. When a
record cannot fit, pass the complete object in eightbyte stack slots and leave the
register budget available for later arguments. The [scalar floating-point ABI design](linux-scalar-floating-point.md)
describes how L2 preserves the result of planning INTEGER and SSE locations together.
AAPCS64 keeps its own placement and aggregate paths.

Before the change, the native #231 reproducer `sum({20, 22})` reports an unsupported
System V AMD64 parameter at both the function and call. The regression matrix runs
the native reproducer and a split provider/client in both directions against GCC and
Clang at `-O0` and `-O2`. It checks a three-byte object with nearby canaries, natural
field padding, mixed signed values in a 12-byte result, a fully occupied two-word
result, one- and two-word stack arguments, use of the remaining GPR after rollback,
stack neighbors, and alias isolation. The existing tests cover the scalar,
variadic and Darwin controls.

The pointer-record fixtures check a one-eightbyte record, both orders of a two-eightbyte
pointer/count record, full register exhaustion and rollback with one register available.
They check pointer identity, pointee writes and by-value independence across the call.
A separate native fixture passes a preformed empty list by value with the pinned libycxx
header. Nonempty list formation continues under [#329](https://github.com/yrashk/ycxx/issues/329).

Indirect and variadic ABI work continues in [#196](https://github.com/yrashk/ycxx/issues/196).
Classes with floating-point members are tracked in
[#336](https://github.com/yrashk/ycxx/issues/336). Broader aggregate classification
and nontrivial class passing remain under [#440](https://github.com/yrashk/ycxx/issues/440).
