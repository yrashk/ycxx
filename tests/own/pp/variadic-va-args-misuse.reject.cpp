// [cpp.replace.general]/14: __VA_ARGS__ and __VA_OPT__ shall occur only in the replacement list
// of a function-like macro that uses the ellipsis notation in its parameters.
#define a(x) __VA_ARGS__ //! error:14
#define b __VA_ARGS__ //! error:11
#define c(x) __VA_OPT__(x) //! error:14
#define d(__VA_ARGS__) 1 //! error:11
#define ok(...) __VA_ARGS__ __VA_OPT__(,)
int i;
