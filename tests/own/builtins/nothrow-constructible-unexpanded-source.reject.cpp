template<class... Sources>
constexpr bool value = __is_nothrow_constructible(int, Sources); //! error
