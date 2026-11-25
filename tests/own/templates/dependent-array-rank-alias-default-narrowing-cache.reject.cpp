// Peers diagnose either the dependent alias or the use that demands the default.
template<bool> using Ignore = int;
using RankTwo = int[2][3];
template<class T> using Erase = Ignore<__array_rank(T)>; template<class T, class U = Erase<T>> struct Use {}; Use<RankTwo, int> explicit_int; Use<RankTwo> omitted; //! error
