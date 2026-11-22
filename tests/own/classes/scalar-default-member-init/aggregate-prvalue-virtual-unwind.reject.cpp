// REQUIRES: linux
// PARITY: #309 Native opaque virtual dispatch is currently supported only on x86-64 Linux.
// XFAIL: clang
// XFAIL: gcc

struct Interface { virtual int read(); };
Interface* source() noexcept;
static_assert(!noexcept(source()->read()));

struct Record { int value = source()->read(); };
int virtual_default() { return Record{}.value; } //! error
