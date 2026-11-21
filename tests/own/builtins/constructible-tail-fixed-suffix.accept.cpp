template<class... Args>
constexpr bool suffix = __is_constructible(int, Args..., double);

template<class... Args>
constexpr bool middle = __is_constructible(int, Args..., double, Args...);

static_assert(suffix<>);
static_assert(!suffix<short>);
static_assert(middle<>);
static_assert(!middle<short>);
