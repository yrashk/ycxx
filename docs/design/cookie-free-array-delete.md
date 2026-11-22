# Cookie-free scalar array deletion

The native Linux x86-64 slice uses the usual global `operator delete[](void*)` for one-dimensional
arrays of integral or ordinary pointer elements. These types are trivially destructible and
default-aligned. Unsized deallocation permits the Itanium ABI's cookie-free representation.
The expression records the canonical deallocator entity. The same entity represents a written
replacement visible in the translation unit. Tagged overloads remain eligible for explicit calls.
[Delete expressions](https://eel.is/c++draft/expr.delete) · [Itanium array cookies](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#array-cookies)

IR generation evaluates the typed pointer once and tests it for null. For a non-null value, the
stateful call releases the entire allocation at its original address. This call is the native
storage-lifetime effect for the allocation, including zero-length arrays. An element lifetime
marker would require a designated element, which a zero-length allocation does not have.
[Object lifetime](https://eel.is/c++draft/basic.life)

The backend enables this lowering for its Linux x86-64 function query. The constant evaluator
builds its own IR and continues to diagnose array deletion. Native array creation and broader
deletion are tracked in [#293](https://github.com/yrashk/ycxx/issues/293). Constant-evaluated
allocation and deletion are tracked in [#321](https://github.com/yrashk/ycxx/issues/321).
