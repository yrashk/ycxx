// [cpp.replace.general]/12, /13, [cpp.subst]/2: in a macro whose parameter list ends in `...`,
// the arguments matching the ellipsis, with the commas between them, form the variable argument
// that __VA_ARGS__ is replaced by; it may hold zero, one or several arguments, and commas
// and parentheses inside it are kept.
#define all(...) [__VA_ARGS__]
#define call(f, ...) f(__VA_ARGS__)
#define count_ish(...) <__VA_ARGS__|__VA_ARGS__>
all();
all(1);
all(1, 2, 3);
all(1,2,3);
all(a , b);
all((x, y), z);
all(,);
all(, a ,);
call(g);
call(g, 1);
call(g, 1, 2);
count_ish(p, q);
