struct Later;

template<class Target, class... Args>
constexpr bool constructs = __is_constructible(Target, Args...); //! error

bool force = constructs<void, Later>;
struct Later { int value; };
