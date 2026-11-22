// XFAIL: clang
// XFAIL: gcc

struct Interface { virtual int read(); };
Interface* source() noexcept;
static_assert(!noexcept(source()->read()));

struct Record { int value = source()->read(); };
int virtual_default() { return Record{}.value; } //! error
