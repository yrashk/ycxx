// [stmt.if], [temp.inst], [expr.prim.lambda.closure]
template<class F> constexpr void dispatch(F&&) {}

template<class T>
constexpr void bridge() {
  if !consteval {
  } else {
    dispatch([] {});
  }
}

extern "C" void negated_entry() {
  bridge<int>();
}
