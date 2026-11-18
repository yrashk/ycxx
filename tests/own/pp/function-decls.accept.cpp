// [cpp.replace.general], [cpp.subst], [cpp.stringize], [cpp.concat]: function-like macros used
// to write declarations: arithmetic helpers with arguments containing parentheses, names formed
// by `##`, a string formed by `#`, and a function-like name not followed by `(` left alone.
#define SQUARE(x) ((x) * (x))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define DECLARE(name, value) int var_ ## name = value;
#define NAME_OF(x) #x
#define GETTER(name) int get_ ## name() { return var_ ## name; }
#define id(x) x
DECLARE(one, 1)
DECLARE(nine, SQUARE(1 + 2))
DECLARE(big, MAX(SQUARE(2), (5 - 1)))
GETTER(one)
GETTER(nine)
const char* label = NAME_OF(var_one + var_nine);
int id = 4;
int twice() { return id(id) * 2 + MAX(get_one(), get_nine()); }
