// #1053 [meta.unary.prop] requires completeness when the initializer is instantiated.
union Pending;

template<class T> inline constexpr bool lifetime_v = __builtin_is_implicit_lifetime(T); //! error
constexpr bool value = lifetime_v<volatile Pending>;
