template<class... Args>
constexpr bool repeated = __is_constructible(int, Args..., Args...);

static_assert(repeated<>);
static_assert(!repeated<double&>);
static_assert(!repeated<short, long>);
