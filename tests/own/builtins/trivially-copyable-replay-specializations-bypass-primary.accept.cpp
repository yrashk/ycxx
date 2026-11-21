template<bool> struct Flag {};
template<class T> struct Check : Flag<__is_trivially_copyable(T)> {};
template<class T> constexpr bool copied = __is_trivially_copyable(T);

struct Forward;
template<> struct Check<Forward> : Flag<false> {};
template<> constexpr bool copied<Forward> = false;

Flag<false>* selected(Check<Forward>* value) { return value; }
static_assert(!copied<Forward>);
