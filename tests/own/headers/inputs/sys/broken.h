// A system header (found through -isystem): its warning is not reported; its error is, and so is
// its #warning, which Clang and GCC report from a system header too.
#pragma once
inline int multichar() { return 'gh'; }
#warning reported: a warning directive in a system header //! warning
int broken = undeclared_in_system_header; //! error
