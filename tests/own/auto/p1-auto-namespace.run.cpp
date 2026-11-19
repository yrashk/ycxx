// θ step 1 (#279), spec-only: `auto` variables at namespace scope, in a named namespace, and `static`
// ([dcl.spec.auto.general], [dcl.type.auto.deduct], [basic.start.static]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

auto g = 17;
static auto sg = 'g';
namespace n {
  auto nl = 100L;
}

static_assert(same<decltype(g), int>::value);
static_assert(same<decltype(sg), char>::value);
static_assert(same<decltype(n::nl), long>::value);

int main() {
  if (g != 17) return 1;
  if (sg != 'g') return 2;
  if (n::nl != 100) return 3;
  g = 18;
  return 0;
}
