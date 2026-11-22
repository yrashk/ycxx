# Linux AMD64 scalar floating-point calls and operations

This slice moves binary32 `float` and binary64 `double` values through fixed-prototype direct
calls and their definitions. It covers typed constants, object loads and stores, selection and
control-flow copies, call arguments and call results (#509). The subsequent
[direct variadic slice](linux-floating-variadic-calls.md) adds binary32-to-binary64 widening.
The signature check excludes x87 `long double`, floating aggregates, and floating operands or
results in indirect calls. Wider aggregate classification remains [#440](https://github.com/yrashk/ycxx/issues/440).

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
The native `<cfloat>` probe also exercises a direct member call and scalar floating returns
deduced from `auto` in a free function and `decltype(auto)` in template specializations.

## Runtime arithmetic

After the front end applies the usual arithmetic conversions, L2 verifies that each arithmetic
node has operands and a result of one floating type. The Linux AMD64 emitter loads the left
operand into XMM0 and uses the right operand's home for scalar ADD, SUB, MUL or DIV. It stores
each result at the L2 width. Binary32 selects the SS form and binary64 selects the SD form.
The instructions inherit the caller's MXCSR controls and naturally update the sticky status
flags. Unary negation XORs the sign bit in an integer scratch register, so signed zero and NaN
payloads are preserved apart from the sign, and MXCSR is untouched.
[Usual arithmetic conversions](https://eel.is/c++draft/expr.arith.conv) ·
[Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)

Astra reviewed these choices against the language clauses, AMD64 psABI and Intel SDM. The
mixed-object tests pass runtime bit patterns to ycxx-generated definitions and verify operand
order, width-sensitive rounding, signed zero, subnormals, signaling and quiet NaN negation,
and the caller's MXCSR controls and flags.

## Runtime comparisons

The six plain L2 predicates compare binary32 with UCOMISS and binary64 with UCOMISD.
An unordered comparison sets ZF, PF and CF. Equality combines SETE with SETNP, and inequality
combines SETNE with SETP. Both tests read the comparison flags before the combine changes them.
SETA and SETAE accept only ordered greater and greater-or-equal values. Reversing the already
evaluated operands gives ordered less and less-or-equal. The existing result store normalizes
the predicate to `i1`. L2 already represents conversion to `bool` as inequality with a zero of
the same width, so both zeros are false and NaN is true.
[Relational operators](https://eel.is/c++draft/expr.rel) ·
[Equality](https://eel.is/c++draft/expr.eq) · [Boolean conversion](https://eel.is/c++draft/conv.bool) ·
[Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)

Astra reviewed the ordered and unordered flag outcomes against the Intel SDM. Comparisons run
under the caller's MXCSR controls. The mixed-object tests check values at the default control
settings and verify that controls survive the calls. Strict exception-status behavior, other
numeric conversions, absolute value and x87 `long double` remain [#201](https://github.com/yrashk/ycxx/issues/201).
