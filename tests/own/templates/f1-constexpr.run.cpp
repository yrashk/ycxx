// ζ f1 (#88): constexpr function templates evaluated in constant expressions: a static_assert,
// an array bound, a non-type template argument of another template, a constexpr variable
// ([dcl.constexpr], [expr.const], [temp.arg.nontype], [temp.inst]).
// EXIT: 0
template<class T>
constexpr T square(T x) { return x * x; }

template<class T, int N>
constexpr T sum_to(T start) {
  T s = 0;
  for (int i = 0; i < N; ++i) s += start + i;
  return s;
}

template<int N>
constexpr int times(int x) { return x * N; }

static_assert(square(7) == 49);
static_assert(square('\3') == 9);
static_assert(sum_to<long, 4>(1L) == 10L);
static_assert(times<square(3)>(2) == 18);

constexpr int k = square(4);

int main() {
  int buf[square(3)];
  for (int i = 0; i < square(3); ++i) buf[i] = i;
  if (buf[8] != 8) return 1;
  if (k != 16) return 2;
  if (times<k>(1) != 16) return 3;
  int n = 5;
  if (square(n) != 25) return 4;
  if (sum_to<int, square(2)>(n) != 26) return 5;
  return 0;
}
