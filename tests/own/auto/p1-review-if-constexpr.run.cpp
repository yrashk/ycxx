// θ step 1 (#279), from the review: a return statement in a discarded `if constexpr` branch
// deduces nothing ([stmt.if]/2); in a template each instantiation deduces from the branch it
// keeps ([dcl.spec.auto.general]/12).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };
auto none() { if constexpr (false) return 1; }
auto pick() { if constexpr (true) return 1; else return 2L; }
template<class T> auto by_size(T) { if constexpr (sizeof(T) == 4) return 1; else return 2L; }
static_assert(same<decltype(none()), void>::value);
static_assert(same<decltype(pick()), int>::value);
int main() {
  none();
  if (pick() != 1) return 1;
  static_assert(same<decltype(by_size(1)), int>::value);
  static_assert(same<decltype(by_size(1L)), long>::value);
  return by_size(1) + by_size(1L) == 3 ? 0 : 2;
}
