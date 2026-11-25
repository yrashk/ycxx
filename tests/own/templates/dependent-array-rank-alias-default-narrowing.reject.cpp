// Peers diagnose either the dependent alias or the use that demands the default.
template<bool> using Ignore = int;
template<class T> using Erase = Ignore<__array_rank(T)>; template<class T, class U = Erase<T>> struct Use {}; Use<int[2][3]> omitted; //! error
