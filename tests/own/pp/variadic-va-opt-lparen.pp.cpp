// [cpp.subst]/3 (example): __VA_OPT__'s content is rescanned with the rest of the replacement,
// so a function-like name in it can take its arguments from tokens after the __VA_OPT__, and a
// `(` produced by a macro argument counts.
#define LPAREN() (
#define G(Q) 42
#define F(R, X, ...) __VA_OPT__(G R X) )
int x = F(LPAREN(), 0, <:-);
