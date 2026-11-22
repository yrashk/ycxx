template<bool> struct Result {};
template<class T> constexpr bool Enabled = false;
template<class T> extern bool Later;
template<class T> using Choose = Result<Enabled<T> && Later<T>>;
template<class T, class U> auto pick(U*) -> Choose<T>;
template<class T> long pick(...);
static_assert(__is_same(decltype(pick<int>(0)), long));
template<> bool Later<int> = true; //! error
