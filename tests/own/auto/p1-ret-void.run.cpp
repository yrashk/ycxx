// θ step 1 (#279), spec-only: an `auto` function with no return statement, or only `return;`, deduces void;
// so does `return` of a void expression ([dcl.spec.auto.general], [dcl.type.auto.deduct],
// [stmt.return]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int counter = 0;
auto bump() { ++counter; }
auto early(int v) {
  if (v > 0) return;
  counter += 10;
  return;
}
auto relay() { return bump(); }
decltype(auto) relay2() { return bump(); }

static_assert(same<decltype(bump()), void>::value);
static_assert(same<decltype(early(1)), void>::value);
static_assert(same<decltype(relay()), void>::value);
static_assert(same<decltype(relay2()), void>::value);

int main() {
  bump();
  early(1);
  early(0);
  relay();
  relay2();
  return counter == 13 ? 0 : 1;
}
