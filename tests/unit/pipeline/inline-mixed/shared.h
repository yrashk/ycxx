// #124: inline definitions shared by two units, next to every other kind of content ycxx
// writes: string literals, dynamic initialization, a guarded block-scope static, pointers in
// data and floating constants. An object with a weak definition lets the linker split its
// sections at symbols (MH_SUBSECTIONS_VIA_SYMBOLS), so the rest must survive that, and
// dead stripping.
inline int shared_fn(int x) { return x * 3; }
inline const int table[4] = {10, 20, 30, 40};
inline int counter = 5;
inline int zeros[8];
inline long shared_twice(long x) { return x + x; }
