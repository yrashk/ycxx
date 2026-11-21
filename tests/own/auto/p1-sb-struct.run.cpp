// θ step 1 (#279), spec-only: binding to a class with public non-static data members names them in
// declaration order; `auto` copies the object, `auto&` refers to it ([dcl.struct.bind]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

struct P {
  int x;
  char c;
  long l;
};

P make() { return P{1, 'b', 3L}; }

int main() {
  P p{7, 'a', 9L};
  auto [x, c, l] = p;
  static_assert(same<decltype(x), int>::value);
  static_assert(same<decltype(c), char>::value);
  static_assert(same<decltype(l), long>::value);
  if (x != 7 || c != 'a' || l != 9) return 1;
  x = 70;
  if (p.x != 7) return 2;
  auto& [rx, rc, rl] = p;
  rx = 8;
  rl = 90L;
  if (p.x != 8 || p.l != 90) return 3;
  auto [mx, mc, ml] = make();
  if (mx != 1 || mc != 'b' || ml != 3) return 4;
  return 0;
}
