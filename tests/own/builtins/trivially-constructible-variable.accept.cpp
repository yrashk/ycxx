template<class Source>
using Borrow = const Source&;

template<class Target, class... Sources>
constexpr bool unpacked = __is_trivially_constructible(Target, Sources...);

template<class... Sources>
constexpr bool prefixed = __is_trivially_constructible(short, unsigned, Borrow<Sources>...);

template<class... Sources>
constexpr bool doubled = __is_trivially_constructible(int, Borrow<Sources>..., Sources...);

static_assert(unpacked<double>);
static_assert(unpacked<double, const int&>);
static_assert(!unpacked<double, int, float>);
static_assert(!unpacked<int&>);
static_assert(!unpacked<const void, int>);
static_assert(prefixed<>);
static_assert(!prefixed<bool>);
static_assert(doubled<>);
static_assert(!doubled<long>);
