# Linux AMD64 integer aggregate calls (#440)

The first slice covers fixed-prototype direct calls to ordinary free functions. By-value
parameters and results can have a complete, nonempty, flat record layout of 1–16 bytes,
with alignment up to eight. The record must be trivial for calls under DECISIONS §1.6.
Its non-static data members have naturally aligned, non-overlapping integral scalar
layouts. The signature check rejects records with bases, bit-fields, unions, nested
classes or arrays, and fields requiring other ABI classes. Aggregate calls through a
function pointer, calls with an ellipsis, and member or template functions stay outside
this slice.

The front end records when a layout attribute or active packing directive makes the
computed natural layout uncertain. The by-value ABI check rejects those shapes even
if their computed offsets fit, since a peer can apply the layout directive. This check
uses the pragma state at the definition, including simple push/pop and explicit reset.
Actual packed layout remains tracked in [#307](https://github.com/yrashk/ycxx/issues/307).

Astra checked the scope and calling convention against the public
[System V AMD64 psABI](https://gitlab.com/x86-psABIs/x86-64-ABI) §§3.2.2–3.2.3,
the [Itanium C++ ABI](https://itanium-cxx-abi.github.io/cxx-abi/abi.html)
§§3.1.2.3 and 3.1.3 and the pinned [expr.call], [class.copy.ctor] and
[class.temporary]. Each laid-out field contributes INTEGER to the eightbytes its
bytes occupy. Padding contributes NO_CLASS. Preserve the real little-endian byte
offsets and bound each load or store by the object size. INTEGER results use RAX
and RDX in eightbyte order.

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
register budget available for later arguments. List the selected register words
first in L2 and the stack words consecutively in source order. When stack words
exist and the register list is short, fill the intervening unused slots to match
the scalar emitter's six-register boundary. All supported stack slots have
eightbyte alignment. The existing frame keeps the call boundary 16-byte aligned.
AAPCS64 keeps its own placement and aggregate paths.

Before the change, the native #231 reproducer `sum({20, 22})` reports an unsupported
System V AMD64 parameter at both the function and call. The regression matrix runs
the native reproducer and a split provider/client in both directions against GCC and
Clang at `-O0` and `-O2`. It checks a three-byte object with nearby canaries, natural
field padding, mixed signed values in a 12-byte result, a fully occupied two-word
result, one- and two-word stack arguments, use of the remaining GPR after rollback,
stack neighbors, and alias isolation. The existing tests cover the scalar,
variadic and Darwin controls.

Indirect and variadic ABI work continues in [#196](https://github.com/yrashk/ycxx/issues/196).
Classes with floating-point members are tracked in
[#336](https://github.com/yrashk/ycxx/issues/336). Broader aggregate classification
and nontrivial class passing remain under [#440](https://github.com/yrashk/ycxx/issues/440).
