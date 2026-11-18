// [cpp.replace.general]/10, /12: a function-like macro name is replaced only when the next
// preprocessing token is `(`, which may come on a later line since new-lines inside an
// invocation are ordinary whitespace. Not followed by `(`, the name is left alone, also when
// the `(` would only come from replacing a macro that follows it. (A `(` on a later line ends
// its line here: Clang and GCC differ on the line of the tokens after such an invocation.)
#define f(x) <x>
#define LP (
#define call f
int f;
f + 1;
f;
(f)(1);
f
(2)
;
f

(3)
;
f LP 4);
call(5);
call;
f
