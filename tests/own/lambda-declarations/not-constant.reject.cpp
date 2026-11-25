// A constexpr static member whose lambda leaves an array's bounds ([expr.const]).
struct A {
  static constexpr int m = [] { int a[3] = {}; return a[5]; }();  //! error
};
