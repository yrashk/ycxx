// A member-declaration needs its ';' ([class.mem.general]). Compilers point at the end of the
// declarator, where the ';' was expected (column left open: Clang and GCC differ by one).
struct S {
  int x //! error
};
