// [expr.const], [temp.arg.nontype], [temp.variadic], [class.derived]
template<unsigned N> struct Constant { static constexpr unsigned value = N; };
template<unsigned N> using Base = Constant<N>;

namespace metrics {
namespace widths {
template<class First, class... Rest>
consteval unsigned score() { return sizeof(First) + sizeof...(Rest); }

template<class First, class... Rest>
struct Unqualified : Base<score<First, Rest...>()> {};
}
}

template<class First, class... Rest>
struct Qualified : Base<metrics::widths::score<First, Rest...>()> {};

static_assert(Qualified<char>::value == 1);
static_assert(Qualified<char, int>::value == 2);
static_assert(Qualified<char, int, char>::value == 3);
static_assert(Base<metrics::widths::score<char, int>()>::value == 2);
static_assert(metrics::widths::Unqualified<char, int>::value == 2);
