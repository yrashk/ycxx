// θ step 1 (#279), spec-only: several return statements in an `auto` function deduce the same type
// ([dcl.spec.auto.general], [dcl.type.auto.deduct], [stmt.return]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

auto sign(int v) {
  if (v < 0) return -1;
  if (v == 0) return 0;
  return 1;
}

auto pick(bool b, char x, char y) {
  if (b) return x;
  return y;
}

int a = 1, b = 2;
auto& choose(bool first) {
  if (first) return a;
  return b;
}

static_assert(same<decltype(sign(0)), int>::value);
static_assert(same<decltype(pick(true, 'a', 'b')), char>::value);
static_assert(same<decltype(choose(true)), int&>::value);

int main() {
  if (sign(-5) != -1 || sign(0) != 0 || sign(9) != 1) return 1;
  if (pick(false, 'a', 'b') != 'b') return 2;
  choose(false) = 20;
  if (b != 20) return 3;
  return 0;
}
