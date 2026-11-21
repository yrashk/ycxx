template<class Target, class... Args>
constexpr bool constructs = __is_constructible(Target, Args...);

static_assert(constructs<int>);
static_assert(constructs<int, double&>);
static_assert(!constructs<int, short, long>);
static_assert(!constructs<void>);
