# Basic asm for Linux x86-64 breakpoints

The statement forms `asm("int3");` and GNU `asm volatile("int3");` cover the
x86 breakpoint in libycxx's freestanding provider. The parser requires a
complete ordinary string literal and semicolon. It checks the syntax of a
discarded `if constexpr` arm with the same rules as an active arm.
[#950](https://github.com/yrashk/ycxx/issues/950) ·
[[dcl.asm]](https://eel.is/c++draft/dcl.asm)

The AST carries the decoded literal into the existing stateful L1 `asm`
operation. The constant evaluator rejects a path that executes it.
[[expr.const]](https://eel.is/c++draft/expr.const)

Native lowering checks the full text on the target's reachable L1 path. On
Linux x86-64, exactly `int3` with no operands or value results becomes a
stateful L2 instruction. The emitter writes one byte, `0xCC`. Execution can
resume at the following instruction after a debugger or returning signal
handler handles the trap. The L1 and L2 state values keep the effect ordered
within its control path.
[Intel instruction reference](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)

Other instructions and targets are part of
[#190](https://github.com/yrashk/ycxx/issues/190) and
[#659](https://github.com/yrashk/ycxx/issues/659).
