// XFAIL: clang Clang permits a pack in the target position
template<class... Types>
constexpr bool value = __is_nothrow_constructible(Types...); //! error
