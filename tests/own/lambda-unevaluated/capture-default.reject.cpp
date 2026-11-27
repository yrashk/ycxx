// [expr.prim.lambda.capture]/3: a lambda that is not local to a block has no capture-default,
// in an unevaluated operand too (#1549).
using X = decltype([=] { return 1; }());  //! error
