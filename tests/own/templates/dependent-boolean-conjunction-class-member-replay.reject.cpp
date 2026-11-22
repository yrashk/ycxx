template<bool> struct Result {};
template<class T> constexpr bool Enabled = false;
template<class T> extern bool Later;
template<class T> using Choose = Result<Enabled<T> && Later<T>>;
template<class T> struct Holder { auto f() -> Choose<T>; };
static_assert(sizeof(Holder<int>) > 0);
template<class T> bool Later = T::missing; //! error
