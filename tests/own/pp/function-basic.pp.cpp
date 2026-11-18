// [cpp.replace.general]/10, /12, [cpp.subst]/1: a function-like macro invocation is its name,
// a `(`, comma-separated arguments and the matching `)`; each parameter in the replacement list
// is replaced by its argument, a parameter may be used several times or not at all, and
// whitespace around and inside arguments does not survive except as single separators.
#define id(x) x
#define add(a, b) ((a) + (b))
#define twice(x) x x
#define first(a, b) a
#define none() nothing
#define spaced( a ,b ) [a|b]
id(1);
add(1, 2);
add(   x   ,y);
twice(t);
first(keep, drop);
none();
none( );
spaced(p,q);
id(  several   tokens  here  );
