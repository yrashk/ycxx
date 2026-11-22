struct Deferred;

template<class... Sources>
constexpr bool value = __is_nothrow_constructible(long, short, Sources...); //! error

constexpr bool instantiate = value<double, Deferred>;
