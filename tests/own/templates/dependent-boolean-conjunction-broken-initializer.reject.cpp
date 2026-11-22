template<bool> struct Result {};
template<class T> constexpr bool Enabled = false;
template<class T> constexpr bool Broken = T{}.absent; //! error
template<class T> using Choose = Result<Enabled<T> && Broken<T>>;
using Witness = Choose<int>;
