// #317, [temp.inst]: a pointer leaves the class member alias and producer body undemanded.
struct Value { long num, den; bool ok; };
struct First { static constexpr long n = -3; };
struct Zero { static constexpr long n = 0; };

template<Value> struct Box { using type = Box; };

namespace detail {
  template<class T, class U> consteval Value make() {
    static_assert(U::n != 0, "zero denominator");
    return {T::n, U::n, true};
  }
}

template<class T, class U> using Mapped = typename Box<detail::make<T, U>()>::type;
template<class T> struct Lazy { using type = Mapped<T, Zero>; };

Lazy<First>* handle;
