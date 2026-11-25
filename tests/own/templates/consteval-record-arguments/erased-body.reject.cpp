// #317, [temp.alias], [temp.arg.nontype]: an erasing alias still needs a valid argument.
struct Value { long num, den; bool ok; };
struct First { static constexpr long n = -3; };
struct Zero { static constexpr long n = 0; };

template<Value> using Erased = int;

namespace detail {
  template<class T, class U> consteval Value make() {
    static_assert(U::n != 0, "zero denominator"); //! error
    return {T::n, U::n, true};
  }
}

template<class T, class U> using Mapped = Erased<detail::make<T, U>()>;
using Demanded = Mapped<First, Zero>;
