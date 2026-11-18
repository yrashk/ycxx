// [cpp.replace.general]/2, [cpp.scope]: a function-like macro may be redefined with the same
// parameters and an identical replacement list; after #undef it may be redefined freely, even
// as an object-like macro.
#define f(a, b) (a + b)
#define f(a, b) (a + b)
#define  f( a,b )  (a + b)
f(1, 2);
#undef f
#define f(x) <x>
f(3);
#undef f
#define f 7
f(4);
