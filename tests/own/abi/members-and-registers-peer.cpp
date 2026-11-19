// The other side of members-and-registers.run.cpp, compiled by Clang.
struct S12 { int a, b, c; };
struct S24 { long a, b, c; };
struct Box { int k; S12 twice() const; S24 big(int m) const; };
S12 Box::twice() const { return {k * 2, k * 2, k * 2}; }
S24 Box::big(int m) const { return {k, m, k + m}; }
// Seven ints, then a 16-byte class: AAPCS64 C.11 puts it on the stack, not split.
struct S16 { long a, b; };
long peer_many(int a, int b, int c, int d, int e, int f, int g, S16 s) { return a + b + c + d + e + f + g + s.a * 100 + s.b * 1000; }
// ycxx's: called from here with registers exhausted.
struct S16b { long a, b; };
long main_many(int a, int b, int c, int d, int e, int f, int g, S16b s);
long peer_calls_many() { return main_many(1, 2, 3, 4, 5, 6, 7, {3, 4}); }
