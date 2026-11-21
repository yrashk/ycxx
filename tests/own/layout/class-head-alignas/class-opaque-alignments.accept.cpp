// [dcl.align]/6 compares alignment requests with a defining declaration.
// XFAIL: clang Clang 23 rejects unequal alignments on two forward declarations.
struct alignas(8) Opaque;
struct alignas(16) Opaque;
Opaque* address;
