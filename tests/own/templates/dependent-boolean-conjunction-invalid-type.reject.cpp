template<bool> struct Result {};
template<class T> constexpr bool Enabled = false;
template<class T> constexpr bool Right = true;
struct NotBoolean {};
template<> constexpr NotBoolean Right<int>{};
template<class T> using Choose = Result<Enabled<T> && Right<T>>; using Witness = Choose<int>; //! error
