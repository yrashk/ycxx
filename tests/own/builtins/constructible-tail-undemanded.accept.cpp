struct Incomplete;

template<bool> struct Flag {};

template<class Target, class... Args>
struct Constructs : Flag<__is_constructible(Target, Args...)> {};

template<class Target, class... Args>
constexpr bool constructs = __is_constructible(Target, Args...);

Constructs<void, Incomplete>* opaque;

static_assert(constructs<int>);
