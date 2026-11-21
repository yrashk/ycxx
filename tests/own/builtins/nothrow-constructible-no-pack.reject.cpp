template<class Type>
constexpr bool value = __is_nothrow_constructible(int, Type...); //! error
