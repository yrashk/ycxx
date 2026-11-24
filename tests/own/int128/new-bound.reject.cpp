// #1172: an array new bound beyond 64 bits is diagnosed as too large ([expr.new]/8), not read from
// its low half.
// FLAGS: -Wno-pedantic
// XFAIL: clang Clang 23 leaves the bound to std::bad_array_new_length at run time
char* f() { return new char[((__int128)1 << 64) + 1]; } //! error
