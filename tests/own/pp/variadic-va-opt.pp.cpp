// [cpp.subst]/3 (examples): __VA_OPT__(content) is replaced by its content when the variable
// arguments contain tokens after macro replacement (so an argument that expands to nothing
// counts as empty), and by nothing otherwise.
#define F(...) f(0 __VA_OPT__(,) __VA_ARGS__)
#define G(X, ...) f(0, X __VA_OPT__(,) __VA_ARGS__)
#define SDEF(sname, ...) S sname __VA_OPT__(= { __VA_ARGS__ })
#define EMP
F(a, b, c);
F();
F(EMP);
G(a, b, c);
G(a, );
G(a);
SDEF(foo);
SDEF(bar, 1, 2);
F(,);
