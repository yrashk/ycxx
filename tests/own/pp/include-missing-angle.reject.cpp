// [cpp.include]/2: #include <h-char-sequence> naming a header that is not found is ill-formed.
// (Both oracles stop after it: one such error per file.)
int before;
#include <no-such-angle-header.h> //! error:10
