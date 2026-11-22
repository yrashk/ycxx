template<bool> struct Result {};
template<class T> constexpr bool Enabled = false;
template<class T> extern bool Later;
template<class T> using Choose = Result<Enabled<T> && Later<T>>;
template<class T> auto pick(T) -> Choose<T>;
using Picked = decltype(pick(0));
template<> bool Later<int> = true; //! error
