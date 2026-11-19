// θ step 1 (#279), spec-only (split from p1-auto-namespace): `auto&` and `const auto*` at namespace scope
// ([dcl.spec.auto.general], [dcl.type.auto.deduct]).
// XFAIL: ycxx a namespace-scope reference needs the address constant of a variable, #101
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

auto g = 17;
auto& rg = g;
namespace n { const auto* np = &g; }
static_assert(same<decltype(n::np), const int*>::value);
static_assert(same<decltype(rg), int&>::value);

int main() {
  rg = 18;
  return g == 18 && *n::np == 18 ? 0 : 1;
}
