// [cpp.concat]/3: the token formed by `##` shall be a valid preprocessing token; both oracles
// diagnose a paste that is not one, on the line of the invocation (columns left out: Clang
// points at the macro name, GCC at the argument).
#define cat(a, b) a ## b
cat(+, -); //! error
cat(x, +); //! error
cat(., .); //! error
cat(1, "s"); //! error
cat(/, /) //! error
int i;
