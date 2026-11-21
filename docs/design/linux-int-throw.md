# Cleanup-free Linux x86-64 `int` throws

The first native throw slice lets a hosted C++ caller catch an `int` thrown by an ordinary
ycxx function on Linux x86-64. The caller's toolchain supplies the final link, the exception
runtime, and the handler. Astra reviewed the exception ABI and the lowering boundary on [#578].

The typed function must qualify for the cleanup-free frame information from [#572]. The
existing lifetime check can walk an explicit throw and its operand. Its result still depends
on all declarations in the unit, including a written `noexcept` or `inline` on a later
redeclaration. The lowering query reads that final result and the runtime symbol identities.

On a runtime-reachable L1 `throw` with no local unwind edge, lowering requires the normalized
exception type to be builtin `int` ([expr.throw], [except.throw]). The operand has already
been evaluated once into SSA. Allocate four bytes with `__cxa_allocate_exception`, store the
operand there, and call `__cxa_throw` with the buffer, the address of `_ZTIi`, and a null
destructor. Finish the block with `unreachable`. If an operand call throws, unwinding leaves
the function before allocation. Both calls use the existing ELF function relocations, and the
type information address uses the existing GOT path for an external data symbol.

L1 runtime reachability determines which throws emit references. The constexpr interpreter
keeps its current handling of a reached throw. The existing native linker requires a provider
for each runtime symbol. Other exception types, rethrow, native handlers, exceptional cleanup,
other targets, and runtime integration with ycxx's own linker remain under [#89].

[Itanium exception handling ABI §2.4](https://itanium-cxx-abi.github.io/cxx-abi/abi-eh.html#cxx-throw) ·
[Itanium C++ ABI §5.1.4.4](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#mangle.special-name)

[#578]: https://github.com/yrashk/ycxx/issues/578
[#572]: https://github.com/yrashk/ycxx/issues/572
[#89]: https://github.com/yrashk/ycxx/issues/89
