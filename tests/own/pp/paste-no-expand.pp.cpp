// [cpp.subst]/1, [cpp.concat]/3, [cpp.rescan]/1: a parameter next to `##` is replaced by its
// argument as written, without macro replacement, and the pasted result is rescanned, so it may
// name a macro, including a function-like one that takes its `(` from what follows. In
// cat(x, cat(1, 2)) the pasted xcat expands to a `cat` met while rescanning cat's own
// replacement, which [cpp.rescan]/2 leaves unreplaced.
#define AB done
#define A first
#define B second
#define cat(a, b) a ## b
#define xcat(a, b) cat(a, b)
#define left(x) x ## 1 x
#define fn(x) <x>
cat(A, B);
xcat(A, B);
cat(f, n)(3);
cat(A, );
left(A);
cat(x, cat(1, 2));
xcat(x, cat(1, 2));
