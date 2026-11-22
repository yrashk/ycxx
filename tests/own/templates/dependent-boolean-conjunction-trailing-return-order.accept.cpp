template<bool> struct Result {};
template<class T> constexpr bool Enabled = false;
template<class T> extern bool Later;
template<class T> using Choose = Result<Enabled<T> && Later<T>>;
template<class T> auto pick(T, typename T::missing) -> Choose<T>;
long pick(...);
static_assert(__is_same(decltype(pick(0, 0)), long));
template<class T> bool Later = T::missing;
