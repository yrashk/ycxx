// [cpp.include]/2-3: a quoted #include whose file is found neither by the quoted search nor
// by the <...> search is ill-formed. (Both oracles stop after it: one such error per file.)
int before;
#include "inputs/no-such-header.h" //! error:10
