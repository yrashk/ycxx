// [expr.prim.lambda.capture]/3: a lambda that is not local to a block has no capture-default (#338).
struct A {
  static constexpr int k = 5;
  static constexpr int m = [&] { return k; }();  //! error
};
