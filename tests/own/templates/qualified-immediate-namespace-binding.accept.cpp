// [basic.lookup.qual], [expr.const], [temp.arg.nontype], [class.derived]
template<unsigned N> struct Constant { static constexpr unsigned value = N; };
template<unsigned N> using Base = Constant<N>;

namespace east {
template<class... Ts> consteval unsigned score() { return sizeof...(Ts) + 7; }
}
namespace west {
template<class... Ts> consteval unsigned score() { return sizeof...(Ts) + 19; }
}
template<class... Ts>
struct East : Base<east::score<Ts...>()> {};
template<class... Ts>
struct West : Base<west::score<Ts...>()> {};

static_assert(East<int, char>::value == 9);
static_assert(West<int, char>::value == 21);
