template<bool> using Ignore = int;
template<class T> using Erase = Ignore<__array_rank(T)>;
template<class T, class U = Erase<T>> struct Use {};

using RankOne = int[2];
using RankTwo = int[2][3];
Use<RankOne> omitted;
Use<RankTwo, int> explicit_int;
static_assert(__is_same(Use<RankOne>, Use<RankOne, int>));
