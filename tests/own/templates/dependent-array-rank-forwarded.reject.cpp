// [temp.arg.nontype]: forwarding preserves the conversion to the default's parameter type.
// The peers diagnose at different points in the alias chain.
template<int> struct Item {};
template<class T, bool B=__array_rank(T)> using To=Item<B>; template<class T> using Via=To<T>; Via<char[2][4]> bad; //! error
