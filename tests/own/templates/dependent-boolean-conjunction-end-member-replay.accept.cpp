template<bool> struct Result {};
template<class T> constexpr bool Enabled = false;
template<class T> extern bool Later;
template<class T> using Choose = Result<Enabled<T> && Later<T>>;
template<class T> struct Holder { auto f() -> Choose<T>; };
template<class T> extern bool Outer;
template<class T> using Start = Result<Enabled<T> && Outer<T>>;
using Required = Start<int>;
template<class T> bool Outer = sizeof(Holder<T>) > 0;
