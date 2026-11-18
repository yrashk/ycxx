// [cpp.subst]/1, [cpp.replace.general]/10: parameter names are local to the macro: a parameter
// named like an object-like macro is replaced by its argument, not by that macro, and identifiers
// in the replacement list that are not parameters are left for rescanning.
#define x 100
#define y 200
#define f(x) x + y
#define g(x, z) z x
#define h(f) f(1)
#define h2(f) f(1, 2)
f(1);
f(x);
g(a, b);
h(f);
h2(g);
