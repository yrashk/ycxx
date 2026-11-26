// [basic.scope.scope]/3.1, [over.load]
// XFAIL: gcc GCC 16 accepts a redeclaration that [basic.scope.scope]/3.1 makes corresponding.
template <class T>
struct Wrap {
  int select() { return 1; }
  int select() & { return 2; } //! error
};
