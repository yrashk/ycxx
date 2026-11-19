// θ step 1 (#279), spec-only: a function declared `auto f();` and later defined with `auto` is usable after
// the definition; a redeclaration after the definition also uses `auto`
// ([dcl.spec.auto.general], [dcl.type.auto.deduct]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

auto f();
auto f() { return 7; }
auto f();
static_assert(same<decltype(f()), int>::value);

auto& g();
int x = 1;
auto& g() { return x; }

int use() { return f() + g(); }

int main() {
  g() = 3;
  if (use() != 10) return 1;
  return 0;
}
