# Linux AMD64 scalar floating-point calls (#509)

This slice moves binary32 `float` and binary64 `double` values through fixed-prototype direct
calls and their definitions. It covers typed constants, object loads and stores, selection and
control-flow copies, call arguments and call results. Runtime arithmetic, comparisons and
numeric conversions remain [#201](https://github.com/yrashk/ycxx/issues/201). The signature check
keeps x87 `long double`, floating aggregates, and floating operands or results in indirect or
variadic calls outside this slice. Variadic SSE classification remains
[#399](https://github.com/yrashk/ycxx/issues/399), and wider aggregate classification remains
[#440](https://github.com/yrashk/ycxx/issues/440).

Astra checked the design against the [System V AMD64 psABI](https://gitlab.com/x86-psABIs/x86-64-ABI)
§§3.2.2–3.2.3. One typed argument planner assigns the six INTEGER registers and eight XMM
registers independently. Spills occupy eightbyte slots at increasing addresses in argument
order. The existing frame reserves space for the spills and preserves 16-byte pre-call stack
alignment. A floating result uses the low 32 or 64 bits of XMM0.

The [INTEGER-record lowering](linux-integer-aggregates.md) uses the same planner before flattening
aggregates into L2 scalar words. A record that does not fit goes whole on the stack and leaves
the remaining INTEGER budget available for later arguments. L2 lists the allocated INTEGER
words, then allocated SSE scalars, then complete spill groups in source order. If any words
spill, unused INTEGER slots are padded so an INTEGER spill stays on the stack when the emitter
plans the flat list. An SSE spill implies all eight XMM registers are already allocated. Scalar-only
signatures retain their source order. The AArch64 path retains its existing lowering.

The emitter homes incoming float and double values at their declared width. It loads the
outgoing registers from those homes after evaluation and immediately homes the XMM0 result.
Floating loads, stores and copies move exactly four or eight bytes. A four-byte stack argument
uses the low four bytes of its eightbyte slot. Integer scratch registers can carry the value's
bits within a frame.

Mixed-object tests exchange arguments in both producer directions, covering register
exhaustion in either order, odd and even spill counts, and an INTEGER rollback followed by
SSE and a remaining GPR argument. Width witnesses check exact bit patterns, adjacent guards
and call results retained across a second call. Cold and daemon builds exercise the same CLI.
The native `<cfloat>` probe also exercises a direct member call and template specializations
returning scalar floating values.
