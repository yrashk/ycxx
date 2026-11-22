// YCXX-only coverage gate for newly replayed potentially throwing defaults (#818, #853).
// XFAIL: clang Clang supports unwinding through these aggregate initializations.
// XFAIL: gcc GCC supports unwinding through these aggregate initializations.
int foreign();
struct Throws { int value = foreign(); };

static_assert(!noexcept(Throws{}));
int named() { Throws object{}; return object.value; }
Throws braced_return() { return {}; } //! error
int temporary() { return Throws{}.value; } //! error

struct Allocates { int* value = new int[2]; };
Allocates array_return() { return {}; } //! error
