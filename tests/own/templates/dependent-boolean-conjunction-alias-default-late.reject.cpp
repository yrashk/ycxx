template<bool> struct Result {};
template<class T> constexpr bool Enabled = false;
template<class T> extern bool Later;
template<class T> using Inner = Result<Enabled<T> && Later<T>>;
template<class T, class U = Inner<T>> using Choose = U;
static_assert(__is_same(Choose<int>, Result<false>));
template<> bool Later<int> = true; //! error
