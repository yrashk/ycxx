// [cpp.include], [cpp.replace.general]: a header's declarations and object-like macros are
// usable in the includer; a macro expands to an expression, a type or a string literal.
#include "inputs/decls.h"
#define TWO 2
#define INT int
#define ANSWER BASE + TWO
INT f() { return ANSWER; }
int g(int x) { return x * TWO + BASE; }
const char *greeting = GREETING;
int value = f() + g(TWO);
