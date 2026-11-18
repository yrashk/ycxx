// ζ f1 (#88): recursive function templates: fact<T> calling itself for several T (fact<char>
// calls fact<int>, since n - 1 promotes), gcd<T>, and tri<N> instantiating tri<N - 1> down to
// tri<0>, which names itself; a constexpr one evaluated in a static_assert ([temp.inst],
// [temp.deduct.call], [dcl.constexpr]).
// EXIT: 0
template<class T>
constexpr T fact(T n) {
  if (n <= 1) return 1;
  return n * fact(n - 1);
}

template<class T>
T gcd(T a, T b) {
  if (b == 0) return a;
  return gcd(b, a % b);
}

template<int N>
int tri() {
  if (N <= 0) return 0;
  return N + tri<(N > 0 ? N - 1 : 0)>();
}

static_assert(fact(5) == 120);
static_assert(fact(10L) == 3628800L);

int main() {
  if (fact(5) != 120) return 1;
  if (fact(12L) != 479001600L) return 2;
  if (fact('\5') != 120) return 3;
  if (fact(6u) != 720u) return 4;
  if (fact(0) != 1) return 5;
  if (gcd(48, 18) != 6) return 6;
  if (gcd(1071L, 462L) != 21L) return 7;
  if (tri<4>() != 10) return 8;
  if (tri<0>() != 0) return 9;
  if (tri<10>() != 55) return 10;
  return 0;
}
