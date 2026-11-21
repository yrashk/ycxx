struct Deferred;

template<class... Sources>
constexpr bool value = __is_trivially_constructible(long, short, Sources...); //! error

constexpr bool instance = value<double, Deferred>;
