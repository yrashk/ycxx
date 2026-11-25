// Lambdas in namespace-scope variables' initializers (#338): called at once, as an argument,
// converted, in an array, with an init-capture, constant and dynamic initialization.
static int s = [] { return 4; }();
int t = [] {
  int a = 2;
  return a * 2;
}();
int dynamic();
int u = [] { return dynamic(); }();
int dynamic() { return 9; }
const int c = [](int x) { return x; }(8);
static_assert(c == 8);
int array[] = {[] { return 1; }(), [] { return 2; }()};
int (*pointer)(int) = [](int x) { return x * 2; };
auto called = [] { return 6; }();
inline constexpr auto shared = [](int x) { return x - 1; };
static auto captured = [k = 42] { return k; };
constexpr auto constant = [k = 3](int x) { return k + x; };
static_assert(constant(4) == 7 && shared(2) == 1);
namespace N {
int w = [x = 4] { return x; }();
}
template <class F>
int apply(F f) { return f(10); }
int applied = apply([](int x) { return x + 1; });
int main() {
  int one = 1;
  if (s != 4 || t != 4 || u != 9 || array[0] + array[1] != 3 || pointer(4) != 8 || called != 6) return 1;
  if (shared(one) != 0 || captured() != 42 || constant(one) != 4 || N::w != 4 || applied != 11) return 2;
  return 0;
}
