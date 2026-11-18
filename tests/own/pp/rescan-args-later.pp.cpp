// [cpp.subst]/1, [cpp.rescan]/1: arguments are replaced in isolation, so a function-like macro
// name ending an argument does not take the `(` that follows in the replacement list during
// argument replacement, but does during the rescan of the whole replacement.
#define f(x) <x>
#define call(m) m(1)
#define pass(m) m
#define paren(m, a) m(a)
call(f);
pass(f)(2);
paren(f, 3);
paren(pass(f), 4);
pass(f (5));
