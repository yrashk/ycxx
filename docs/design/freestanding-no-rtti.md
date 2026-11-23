# Disabling runtime type information

The CLI and compilation database share `-fno-rtti`. The effective mode
distinguishes source configurations. Its flag occupies another bit of the
persisted mode byte, so earlier values decode with RTTI enabled. RTTI feature
macros follow the existing implementation coverage policy.

The front end diagnoses checked `typeid` and `dynamic_cast` operations that
require RTTI. `dynamic_cast<cv void*>` still uses offset-to-top. Pointer casts
to the same class or an unambiguous accessible base reuse ordinary pointer
conversions, including their qualification and layout checks.
GCC defers a nondependent downcast in a dormant function template, while Clang
diagnoses it when checking the definition. ycxx checks the cast when its
existing semantic analysis reaches the nondependent operation.

The native Itanium producer keeps its vtable owner, binding, address point,
offset-to-top and function slots. The owned table has a zero typeinfo word in
disabled mode, and the table emits no associated type descriptors or names.
Exception handling keeps the type references required by its own ABI.
Astra reviewed the Linux vtable rule. Clang's arm64 Darwin object uses the
same header layout and zero typeinfo word.

[#982](https://github.com/yrashk/ycxx/issues/982) · [Cast and type information #294](https://github.com/yrashk/ycxx/issues/294) · [Itanium ABI §2.5.2](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vtable-components)
