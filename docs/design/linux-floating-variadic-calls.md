# Linux AMD64 direct floating variadic calls (#399)

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

Only direct calls admit floating variadic signatures and binary64 tail arguments. Variadic
definitions with floating parameters or results, indirect floating variadic calls and native
floating `va_arg` remain [#196](https://github.com/yrashk/ycxx/issues/196). Other numeric floating
operations remain [#201](https://github.com/yrashk/ycxx/issues/201). Fixed INTEGER record calls
continue to use [#440](linux-integer-aggregates.md) and its register rollback.

The mixed-object checks use a native `va_list` consumer for exact values, negative zero,
independent register exhaustion, and mixed stack arguments. An entry observer checks AL before
a prologue, including named SSE arguments with an empty tail and the first spilled double.
