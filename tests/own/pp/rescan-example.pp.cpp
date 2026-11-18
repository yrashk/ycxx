// [cpp.rescan]/1 (example): the replacement of f(2) is rescanned together with the rest of the
// source, so the function-like g at its end takes `(9)` from the source. The draft leaves it
// unspecified whether the f in g's replacement f(9) is replaced (giving 2*9*g) or not (giving
// 2*f(9)); Clang and GCC both replace it, and this test follows them.
#define f(a) a*g
#define g(a) f(a)
f(2)(9)
