// [cpp.replace.general]/12: the number of arguments in an invocation (one, empty, for `()` when
// the macro has parameters) shall equal the number of parameters, or be at least the number of
// named parameters of a variadic macro. Columns are left out: Clang reports the macro name or
// the extra argument, GCC the closing parenthesis.
#define none() 0
#define one(a) a
#define two(a, b) a b
#define var(a, b, ...) a b
none(1); //! error
none(,); //! error
one(1, 2); //! error
one(,); //! error
two(1); //! error
two(); //! error
two(1, 2, 3); //! error
var(1); //! error
int ok0 = none();
int ok1 = one(1);
int ok2 = one() 2;
int ok3 = two(3,);
int ok4 = two(,4);
int ok5 = var(5,);
int ok6 = var(,6, x, y);
int ok7 = var(7, , z);
