template<class... Args>
constexpr bool from_lvalue = __is_constructible(int, Args&...);

static_assert(from_lvalue<>);
static_assert(from_lvalue<double>);
static_assert(from_lvalue<double&&>);
static_assert(!from_lvalue<short, long>);
