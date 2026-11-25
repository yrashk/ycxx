// A static data member's initializer has no object, nor has a lambda in it ([expr.prim.id.general]).
struct A {
  int x;
  static constexpr int m = [] { return x; }();  //! error
};
