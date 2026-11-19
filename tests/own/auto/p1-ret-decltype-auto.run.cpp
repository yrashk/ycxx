// θ step 1 (#279), spec-only: a `decltype(auto)` return type is decltype of the returned expression: an
// id-expression gives its declared type, a parenthesized lvalue an lvalue reference, a call its
// return type ([dcl.type.auto.deduct], [dcl.type.decltype], [stmt.return]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int g = 1;
int& ref() { return g; }
decltype(auto) by_value() { return g; }
decltype(auto) by_ref() { return (g); }
decltype(auto) forward_ref() { return ref(); }
decltype(auto) sum() { return g + 1; }

static_assert(same<decltype(by_value()), int>::value);
static_assert(same<decltype(by_ref()), int&>::value);
static_assert(same<decltype(forward_ref()), int&>::value);
static_assert(same<decltype(sum()), int>::value);

int main() {
  by_ref() = 5;
  if (g != 5) return 1;
  forward_ref() = 6;
  if (g != 6) return 2;
  if (by_value() != 6) return 3;
  if (sum() != 7) return 4;
  return 0;
}
