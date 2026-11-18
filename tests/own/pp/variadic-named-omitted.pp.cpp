// [cpp.replace.general]/12: a variadic macro needs at least as many arguments as named
// parameters, and the variable arguments may be omitted entirely, comma included; then
// __VA_ARGS__ is empty.
#define G(X, ...) f(0, X __VA_ARGS__)
#define two(a, b, ...) [a|b|__VA_ARGS__]
G(a);
G(a,);
G(a, b);
G(a, b, c);
two(1, 2);
two(1, 2,);
two(1, 2, 3, 4);
two(,);
