// θ step 2 (#280), spec-only: a trailing-return-type gives the lambda's return type, converting the
// operands of the return statements ([expr.prim.lambda.general], [expr.prim.lambda.closure],
// [dcl.fct], [stmt.return]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  auto widen = [](int a) -> long { return a; };
  auto w = widen(7);
  static_assert(same<decltype(w), long>::value);
  if (w != 7) return 1;
  auto mixed = [](bool b) -> long {
    if (b) return 1;
    return 'a';
  };
  if (mixed(true) != 1 || mixed(false) != 97) return 2;
  int x = 3;
  auto ref = [](int& r) -> int& { return r; };
  ref(x) = 30;
  if (x != 30) return 3;
  auto narrow = [](int v) -> char { return v; };
  auto n = narrow(65);
  static_assert(same<decltype(n), char>::value);
  if (n != 'A') return 4;
  auto empty = []() -> void {};
  empty();
  return 0;
}
