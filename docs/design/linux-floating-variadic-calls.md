# Linux AMD64 scalar floating variadics (#399, #196)

A direct variadic call may send the scalar arguments and receive the scalar results already
supported for fixed prototypes. A named `float` uses binary32, and the front end promotes an
unnamed `float` to binary64 before ABI placement. The Linux emitter implements the corresponding
L2 `fpext` with SSE2 `CVTSS2SD` from the four-byte source home into an eight-byte result home.
The same instruction supports binary32-to-binary64 widening in other expressions.
[Default argument promotions](https://eel.is/c++draft/expr.call) ·
[Floating-point promotion](https://eel.is/c++draft/conv.fpprom)

Astra reviewed the register assignment against the System V AMD64 psABI. The call carries
`variadic` in L1 and L2 even when its tail is empty. The existing argument planner assigns GPRs,
XMM registers and stack slots. The emitter sets AL from the number of XMM registers actually
assigned, after restoring the arguments from their homes. Named and unnamed arguments contribute.
Stack arguments and results contribute zero. A scalar floating result comes from XMM0 as in a
fixed call. [AMD64 psABI §§3.2.3 and 3.5.7](https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/e1ce098331da5dbd66e1ffc74162380bcc213236/x86-64-ABI/low-level-sys-info.tex)

Direct calls and ordinary free definitions admit scalar floating variadic signatures and
binary64 tail arguments. Indirect floating variadic calls and aggregate variadic signatures
and retrieval remain [#196](https://github.com/yrashk/ycxx/issues/196).
[Scalar floating arithmetic](linux-scalar-floating-point.md#runtime-arithmetic) covers basic
binary32 and binary64 operations. Fixed INTEGER record calls continue to use
[#440](linux-integer-aggregates.md) and its register rollback.

The mixed-object checks use a native `va_list` consumer for exact values, negative zero,
independent register exhaustion, and mixed stack arguments. An entry observer checks AL before
a prologue, including named SSE arguments with an empty tail and the first spilled double.

## Definitions and cursor retrieval

Entry saves the six incoming GPRs and, when AL is nonzero, all eight incoming XMM registers
before homing named values or executing the function body. `va_start` uses the same argument
placement as parameter homing. Its GP offset skips named GPR slots, its FP offset skips named
XMM slots beginning at byte 48, and its overflow pointer starts after the named stack slots.

For `va_arg(ap, double)`, the FP offsets 48 through 160 locate the low eight bytes of each
16-byte XMM slot. A register fetch advances only the FP offset by 16. At offset 176 the fetch
uses the common stack cursor and advances it by eight. An INTEGER register fetch advances only
the GP offset. A binary64 value travels as raw bits through retrieval and storage, and the same
rules apply to cursors provided by native code. `va_copy` copies each position by value.
[AMD64 psABI §3.5.7](https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/e1ce098331da5dbd66e1ffc74162380bcc213236/x86-64-ABI/low-level-sys-info.tex#L2304-2440) ·
[Variadic support](https://eel.is/c++draft/cstdarg.syn)

The native peers check mixed GP and SSE exhaustion, named stack arguments, copied cursors,
and lists crossing the compiler boundary in both directions. They check the sign of zero and
NaN payload bits in scalar arguments, fetched values and results. Floating retrieval uses
`double`, the default promotion type for an unnamed `float`. x87 value support remains in
[#201](https://github.com/yrashk/ycxx/issues/201).
