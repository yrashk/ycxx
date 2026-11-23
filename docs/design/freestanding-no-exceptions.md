# Disabling exception handling

The driver and compilation database use the same `-fno-exceptions` option.
The effective mode distinguishes source configurations for incremental builds.
The persisted language-mode byte keeps the original hostedness bit and uses
one further bit to disable exceptions. Existing values decode with exceptions
enabled. Feature macros follow the existing implementation coverage policy.

In disabled mode, semantic analysis diagnoses `throw`, rethrow and a block
`try` when their enclosing body is checked. A function or generic lambda
template body can retain these constructs in its definition. Instantiation
checks the selected statements. Ordinary non-template discarded statements
and unevaluated operands still receive the diagnostic. Declarations and
queries of `noexcept` keep their language rules.

Native code in disabled mode uses ordinary scope exit and destruction. The
compiler retains cleanup-free frame information for eligible functions. It
omits exceptional cleanup, handler lowering and the associated runtime calls.
The pinned libycxx allocator helper uses the direct factory branch in this mode.

[#980](https://github.com/yrashk/ycxx/issues/980) · [Exception support #89](https://github.com/yrashk/ycxx/issues/89)
