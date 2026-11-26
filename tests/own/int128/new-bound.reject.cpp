// #1172: an array new bound beyond 64 bits is diagnosed as too large ([expr.new]/8), not read from
// its low half.
// FLAGS: -Wno-pedantic
// XFAIL: clang Clang 23 warns and converts the tested bound to size_t
char* f() { return new char[((__int128)1 << 64) + 1]; } //! error
