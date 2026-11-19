// θ step 1 (#279), spec-only: `auto` deduces a class type from a prvalue and from an lvalue of class type;
// `auto&` binds to the object ([dcl.type.auto.deduct], [dcl.init]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

struct P {
  int x, y;
  P(int a, int b) : x(a), y(b) {}
  int sum() const { return x + y; }
};

P make() { return P(1, 2); }

int main() {
  auto p = make();
  static_assert(same<decltype(p), P>::value);
  auto q = P(3, 4);
  auto copy = q;
  copy.x = 30;
  if (q.x != 3) return 1;
  auto& rq = q;
  rq.y = 40;
  if (q.y != 40) return 2;
  const auto& cp = p;
  static_assert(same<decltype(cp), const P&>::value);
  if (cp.sum() != 3) return 3;
  auto pp = &q;
  static_assert(same<decltype(pp), P*>::value);
  if (pp->sum() != 43) return 4;
  return 0;
}
