// [cpp.subst]/1, [cpp.rescan]/1: an argument is fully macro-replaced before substitution, as if
// it formed the rest of the file, and the result is rescanned with the rest of the file, so
// invocations nest, and a macro name produced by substitution can take its `(` from the
// source after the invocation.
#define ONE 1
#define inc(x) (x + 1)
#define dbl(x) (2 * x)
#define apply(m, v) m(v)
#define name(m) m
#define obj ONE
inc(inc(inc(0)));
dbl(inc(ONE));
apply(inc, ONE);
apply(dbl, apply(inc, 2));
name(inc)(3);
name(inc) (4);
inc(obj);
apply(name, inc)(5);
