// [cpp.include]/4: #include pp-tokens whose replacement is < ... > is processed as
// #include <...>; the header lies in an -I directory.
// FLAGS: -Iinputs/angle
#define HEADER <angle.h>
before;
#include HEADER
after;
