// #1053 [meta.unary.prop] requires a complete non-array object type.
union Pending;
constexpr bool value = __builtin_is_implicit_lifetime(Pending); //! error
