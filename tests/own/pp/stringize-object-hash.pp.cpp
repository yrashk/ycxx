// [cpp.stringize]/2, [cpp.subst]: `#` applies only to a parameter, so in a function-like macro
// `#` before a parameter stringizes it while a `#` in an object-like macro is an ordinary token;
// the stringized argument may be a parameter that is also used unstringized.
#define obj # x
#define both(a, b) #a a #b b
#define sep(a) # a #a
obj;
both(1, 2);
sep(t);
