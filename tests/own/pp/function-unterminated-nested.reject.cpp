// [cpp.replace.general]/12: inner parentheses must match before the invocation's own `)`, so an
// invocation whose arguments leave a `(` open is unterminated at the end of the file (column
// left out: Clang reports the macro name, GCC also the end of the line).
#define f(x) x
int i;
f((1, 2) //! error
