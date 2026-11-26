// [basic.scope.scope]/3.1, [over.load], [temp.inst]
// XFAIL: gcc GCC 16 accepts a redeclaration that [basic.scope.scope]/3.1 makes corresponding.
template <class T>
struct Wrap {
  int f(T) & { return 1; }
  int f(int) { return 2; } //! error
};
Wrap<long> distinct;
Wrap<int> corresponding;
