// #317, [temp.res], [temp.expl.spec]: keep definition-time lookup and select
// the specialization for the actual bound types when the alias is demanded.
struct Value { long num, den; bool ok; };
struct First { static constexpr long n = -3; };
struct Second { static constexpr long n = 5; };
struct Third { static constexpr long n = 9; };

template<Value> struct Box { using type = Box; };

namespace detail {
  template<class T, class U> consteval Value make() { return {T::n, U::n, true}; }
}

template<class T, class U> using Mapped = typename Box<detail::make<T, U>()>::type;
template<class T> using Forwarded = Mapped<T, Second>;

template<> consteval Value detail::make<First, Second>() { return {7, -5, false}; }

namespace detail {
  template<class T, class U> requires (sizeof(T) > 0)
  consteval Value make() { return {0, 1, false}; }
}

static_assert(__is_same(Mapped<First, Second>, Box<Value{7, -5, false}>));
static_assert(__is_same(Mapped<Second, First>, Box<Value{5, -3, true}>));
static_assert(__is_same(Forwarded<Third>, Box<Value{9, 5, true}>));
