template<bool> struct Result {};
template<class T> constexpr bool Enabled = false;
template<class T> extern bool Later;
template<class T> using Choose = Result<Enabled<T> && Later<T>>;
template<class T> auto pick(typename T::missing) -> Choose<T>;
template<class T> long pick(...);
static_assert(__is_same(decltype(pick<int>(0)), long));
template<> bool Later<int> = true;
