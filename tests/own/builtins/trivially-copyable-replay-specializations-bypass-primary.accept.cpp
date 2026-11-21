template<bool> struct Flag {};
template<class T> struct Check : Flag<__is_trivially_copyable(T)> {};
template<class T> constexpr bool copied = __is_trivially_copyable(T);

struct Forward;
template<> struct Check<Forward> {
  static constexpr bool selected = false;
};
template<> constexpr bool copied<Forward> = false;

static_assert(!Check<Forward>::selected);
static_assert(!copied<Forward>);
