struct Deferred;
constexpr bool value = __is_nothrow_constructible(int, short, Deferred, long); //! error
