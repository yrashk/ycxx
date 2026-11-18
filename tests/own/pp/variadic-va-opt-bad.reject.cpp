// [cpp.subst]/3, [cpp.concat]/1: __VA_OPT__ shall be followed by a parenthesized replacement
// list that does not contain __VA_OPT__, and `##` shall not begin or end that list (the H1
// example of [cpp.subst]).
#define a(...) __VA_OPT__ x //! error
#define b(...) __VA_OPT__ //! error
#define c(...) __VA_OPT__(x //! error
#define d(...) __VA_OPT__(__VA_OPT__(x)) //! error:27
#define H1(X, ...) X __VA_OPT__(##) __VA_ARGS__ //! error
#define e(X, ...) __VA_OPT__(## X) //! error:30
#define f(X, ...) __VA_OPT__(X ##) //! error:34
int i;
