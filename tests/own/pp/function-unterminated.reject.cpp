// [cpp.replace.general]/12: an invocation is terminated by the `)` matching its `(`; the file
// ending before it is an error, reported on the invocation's line (column left out: Clang
// reports the macro name, GCC also the end of the line).
#define f(x) x
int i;
f(1, //! error
