// θ step 1 (#279), spec-only: a constexpr function with a deduced return type is usable in a constant
// expression after its definition ([dcl.spec.auto.general], [dcl.constexpr], [expr.const]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

constexpr auto cube(int v) { return v * v * v; }
constexpr auto half(long v) { return v / 2; }

static_assert(cube(3) == 27);
static_assert(same<decltype(half(4)), long>::value);
static_assert(half(10) == 5);

int main() {
  constexpr auto c = cube(2);
  int arr[c];
  static_assert(sizeof(arr) == 8 * sizeof(int));
  return c - 8;
}
