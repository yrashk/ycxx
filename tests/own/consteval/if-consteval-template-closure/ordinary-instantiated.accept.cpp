// [stmt.if], [temp.inst], [expr.prim.lambda.closure]
template<class F> constexpr void dispatch(F&&) {}

template<class T>
constexpr void bridge() {
  if consteval {
    dispatch([] {});
  }
}

extern "C" void ordinary_entry() {
  bridge<int>();
}
