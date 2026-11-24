// A class template's member declaration that calls an earlier static member of the same class in
// a decltype operand: the member's deduced return type is its own instantiation's, made while the
// class is instantiated ([dcl.spec.auto.general]/11, [temp.inst], [class.member.lookup]). (#1243)
// EXIT: 0
template <class T> struct ti { using type = T; };

template <class T> struct Own {
  static auto pick(int) { return ti<T>{}; }
  static auto pick(long) { return ti<T*>{}; }
  template <class U> static auto deduced(U) { return ti<U>{}; }
  template <class U> static auto deduced(U*) { return ti<U>{}; }
  static int plain(int);
  static long plain(long);
  static auto uses_plain() { return sizeof(plain(0L)); }
  static auto chain() { return pick(0); }
  static constexpr auto value() { return sizeof(T); }
  using a = typename decltype(pick(0))::type;
  using b = typename decltype(pick(0L))::type;
  using c = typename decltype(deduced(T()))::type;
  using d = typename decltype(deduced((T*)nullptr))::type;
  using e = decltype(plain(0L));
  using f = decltype(uses_plain());
  using g = typename decltype(chain())::type;
  // Called again once the class is complete.
  static int sum() { return int(value()) + int(sizeof(pick(0))); }
};

// Recursion through a member function template with explicit arguments, in a partial specialization too.
template <class... Ts> struct Largest {
  template <class C, class... Rest> static consteval auto pick() {
    if constexpr (((sizeof(C) >= sizeof(Ts)) && ...)) return ti<C>{};
    else return pick<Rest...>();
  }
  using type = typename decltype(pick<Ts...>())::type;
};
template <class T, class U> struct Partial {};
template <class T> struct Partial<T, int> {
  template <class C> static consteval auto pick() {
    if constexpr (sizeof(C) > 1) return ti<C>{};
    else return ti<long>{};
  }
  using type = typename decltype(pick<T>())::type;
  type m = 3;
};

// An overload set of an ordinary class being defined.
struct Plain {
  static long f(long);
  static char f(char);
  using type = decltype(f(0L));
};

template <class A, class B> constexpr bool same = __is_same(A, B);
using X = Own<short>;
static_assert(same<X::a, short> && same<X::b, short*> && same<X::c, short> && same<X::d, short>);
static_assert(same<X::e, long> && same<X::f, decltype(sizeof 0)> && same<X::g, short>);
static_assert(same<Largest<char, long, int>::type, long> && same<Largest<char>::type, char>);
static_assert(same<Partial<char, int>::type, long> && same<Partial<short, int>::type, short>);
static_assert(same<Plain::type, long>);
// A constraint on such a member.
template <class T> constexpr auto select() {
  if constexpr (sizeof(T) == 1) return 1;
  else return ti<T>{};
}
template <class T> concept has_type = requires { typename decltype(select<T>())::type; };
static_assert(has_type<int> && !has_type<char>);

int main() { return Own<long>::sum() + int(Partial<char, int>{}.m) - int(sizeof(long)) - 4; }
