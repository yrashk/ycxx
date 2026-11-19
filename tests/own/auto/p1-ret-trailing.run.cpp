// θ step 1 (#279), spec-only: `auto` with a trailing return type is not deduced; the trailing type is the
// return type and the return operand converts to it ([dcl.spec.auto.general], [dcl.fct]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

auto f() -> int { return 'a'; }
auto g(long v) -> long { return v * 2; }
int x = 3;
auto h() -> int& { return x; }
auto decl_only() -> char;

static_assert(same<decltype(f()), int>::value);
static_assert(same<decltype(g(1)), long>::value);
static_assert(same<decltype(h()), int&>::value);
static_assert(same<decltype(decl_only()), char>::value);

int main() {
  if (f() != 97) return 1;
  if (g(21) != 42) return 2;
  h() = 4;
  if (x != 4) return 3;
  if (decl_only() != 'd') return 4;
  return 0;
}

auto decl_only() -> char { return 'd'; }
