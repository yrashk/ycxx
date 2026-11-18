// [cpp.stringize]/1: in a function-like macro each `#` in the replacement list shall be followed
// by a parameter (or __VA_OPT__ in a variadic macro); in an object-like macro `#` is ordinary.
#define a(x) # y //! error
#define b(x) x # //! error
#define c(x) # # x //! error
#define d(...) # 1 //! error
#define ok # y
int i;
