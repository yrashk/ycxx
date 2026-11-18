// [cpp.rescan]/2: a macro name found while rescanning its own replacement (or the replacement
// of a macro it led to) is not replaced, and such a token is never replaced again.
#define self self
#define x (4 + x)
#define ping pong
#define pong ping
#define outer inner outer
#define inner outer inner
self;
x;
ping;
pong;
outer;
inner;
