// Object-like and function-like macro replacement ([cpp.replace]), stringizing ([cpp.stringize])
// and token pasting ([cpp.concat]), compared with the oracle's -E output.
#define N 42
#define ADD(a, b) ((a) + (b))
#define STR(x) #x
#define CAT(a, b) a##b
int CAT(value, 1) = ADD(N, 1);
const char *s = STR(ADD(1, 2));
