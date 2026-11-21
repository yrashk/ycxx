struct First;
struct Second;

template<class... Args>
constexpr bool constructs = __is_constructible(void, Args...); //! error

bool force = constructs<First, Second>;
