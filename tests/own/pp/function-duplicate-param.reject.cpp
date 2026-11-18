// [cpp.replace.general]/6: an identifier shall be declared as a parameter only once in a
// function-like macro's parameter list.
#define f(a, a) a //! error:14
#define g(x, y, x) x //! error:17
#define h(p, q) p q
int i;
