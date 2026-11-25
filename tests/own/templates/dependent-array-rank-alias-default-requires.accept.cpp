template<bool> using Ignore = int;
template<class T> using Erase = Ignore<__array_rank(T)>;
template<class T, class U = Erase<T>> struct Use {};

using RankOne = int[2];
using RankTwo = int[2][3];
Use<RankTwo, int> explicit_int;

template<class T>
concept UsesDefault = requires { typename Use<T>; };

template<class T>
concept UsesExplicitInt = requires { typename Use<T, int>; };

static_assert(UsesDefault<RankOne>);
static_assert(!UsesDefault<RankTwo>);
static_assert(UsesExplicitInt<RankTwo>);
