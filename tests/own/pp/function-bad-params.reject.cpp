// [cpp.replace.general]/1, /6: a function-like macro's parameter list is a comma-separated list
// of identifiers, optionally ending in `...`, closed by `)` on the directive's line.
#define a(x,) x //! error:13
#define b(1) 1 //! error:11
#define c(x y) x //! error:13
#define d(x //! error
#define e(..., x) x //! error:14
#define f(,x) x //! error:11
#define g(x...) x //! error:12
int i;
