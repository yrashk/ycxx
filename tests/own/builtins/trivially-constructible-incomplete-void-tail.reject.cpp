struct Deferred;
constexpr bool value = __is_trivially_constructible(const volatile void, Deferred); //! error
