// XFAIL: gcc GCC 16 evaluates a call of a written memchr declaration as its builtin. memchr is not constexpr, and an implementation may not make it so ([constexpr.functions]); Clang agrees with ycxx.
// Only __builtin_memchr is a constant expression: a call of a declared `memchr`, the same function
// at run time, is a call of a function that is not constexpr ([expr.const]).
extern "C" const void* memchr(const void*, int, decltype(sizeof(0)));
static_assert(memchr("abc", 'b', 3) != nullptr); //! error
