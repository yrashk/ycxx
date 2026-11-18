// [cpp.include]/4: #include pp-tokens: the tokens are macro-replaced and the result, a
// string-literal, is then processed as #include "...". The macro is replaced through another.
#define HEADER_NAME "inputs/basic.h"
#define WHICH HEADER_NAME
before;
#include WHICH
after;
