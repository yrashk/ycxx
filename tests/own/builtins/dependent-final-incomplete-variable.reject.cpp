// #1042 [meta.unary.prop] requires a complete class type, including a union.
union Pending;

template<class T> inline constexpr bool final_v = __is_final(T); //! error
constexpr bool substituted = final_v<volatile Pending>;
