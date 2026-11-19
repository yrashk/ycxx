// θ step 1 (#279), spec-only: a `static constexpr auto` data member deduces its type from its initializer
// ([dcl.spec.auto.general], [class.static.data], [dcl.constexpr]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

struct S {
  static constexpr auto size = 4;
  static constexpr auto tag = 'S';
  static constexpr auto big = 10L;
  static inline auto counter = 0;
  int data[size];
};

static_assert(same<decltype(S::size), const int>::value);
static_assert(same<decltype(S::tag), const char>::value);
static_assert(same<decltype(S::big), const long>::value);
static_assert(same<decltype(S::counter), int>::value);
static_assert(sizeof(S) == 4 * sizeof(int));

int main() {
  S::counter += 3;
  if (S::size != 4) return 1;
  if (S::tag != 'S') return 2;
  if (S::big != 10) return 3;
  if (S::counter != 3) return 4;
  return 0;
}
