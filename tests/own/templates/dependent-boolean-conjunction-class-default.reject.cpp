template<bool> struct Result {};
template<class T> constexpr bool Enabled = false;
template<class T> extern bool Later;
template<class T> using Choose = Result<Enabled<T> && Later<T>>;
template<class T, class U = Choose<T>> struct WithDefault {};
using Actual = WithDefault<int>;
template<class T> bool Later = T::missing; //! error
