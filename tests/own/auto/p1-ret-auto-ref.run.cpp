// θ step 1 (#279), spec-only: `auto&` and `const auto&` return types deduce a reference to the returned
// lvalue; `auto*` deduces from a returned pointer ([dcl.spec.auto.general],
// [dcl.type.auto.deduct], [stmt.return]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int g = 1;
auto& get() { return g; }
const auto& cget() { return g; }
auto* pget() { return &g; }

static_assert(same<decltype(get()), int&>::value);
static_assert(same<decltype(cget()), const int&>::value);
static_assert(same<decltype(pget()), int*>::value);

int main() {
  get() = 10;
  if (g != 10) return 1;
  if (cget() != 10) return 2;
  *pget() = 11;
  if (g != 11) return 3;
  if (&get() != &g || &cget() != &g) return 4;
  return 0;
}
