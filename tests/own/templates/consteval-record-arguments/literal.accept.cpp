// #317, [temp.arg.nontype], [temp.type]: a qualified immediate call and
// equivalent named and braced records select the same class specialization.
struct Value {
  long num;
  long den;
  bool ok;
};

template<Value> struct Box { using type = Box; };

namespace detail {
  template<class, class> consteval Value make() { return {-3, 5, true}; }
}

template<class T, class U> using Mapped = typename Box<detail::make<T, U>()>::type;

constexpr Value named{-3, 5, true};
using Result = Mapped<int, char>;
static_assert(__is_same(Result, Box<named>));
static_assert(__is_same(Result, Box<Value{-3, 5, true}>));
