// [cpp.pre]/1: # followed by an identifier that does not name a directive, outside a skipped
// group, is a conditionally-supported-directive; Clang and GCC reject unknown names.
#bogus //! error:2
#definee X 1 //! error:2
int i;
