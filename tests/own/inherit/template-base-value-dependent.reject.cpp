// A value-dependent base argument is checked when the specialization is instantiated
// ([temp.inst], [temp.arg.nontype]) (#308).
template<bool B> struct BC { static constexpr bool value = B; };
struct NoValue {};
template<class T> struct Same : BC<T::value> {}; static_assert(Same<NoValue>::value); //! error
struct Runtime { static bool value; };
template<class T> struct Not : BC<!T::value> {}; static_assert(Not<Runtime>::value); //! error
struct Good { static constexpr bool value = true; };
static_assert(Same<Good>::value && !Not<Good>::value);
