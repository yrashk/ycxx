// [cpp.rescan]/2: a function-like macro's own name found while rescanning its replacement, or
// the replacement of a macro it invoked, is not replaced, and stays unreplaceable later even in
// a context where it would otherwise be replaced.
#define f(x) f(x + 1)
#define g(x) h(x)
#define h(x) g(x)
#define self(x) x
#define again(x) x(again)(x)
f(0);
f(f(0));
g(1);
h(2);
self(self(3));
self(self)(4);
again(self);
