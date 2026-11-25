// [dcl.array], [meta.unary.prop.query], [temp.dep.constexpr]
#if !__has_builtin(__array_rank)
#error missing __array_rank
#endif

using Size = decltype(sizeof(0));

template<Size N> struct Value { static constexpr Size value = N; };
template<int N> struct IntValue { static constexpr int value = N; };
template<class T> struct Rank : Value<__array_rank(T)> {};
template<class T> using RankAlias = Value<__array_rank(T)>;
template<class T> struct IntRank : IntValue<__array_rank(T)> {};
template<class T> inline constexpr Size rank_v = __array_rank(T);

struct Incomplete;
using Matrix = const int[][2][3];
static_assert(__is_same(decltype(__array_rank(Matrix)), Size));
static_assert(__array_rank(Matrix) == 3);
static_assert(__array_rank(Incomplete[4]) == 1);
static_assert(__array_rank(Incomplete[]) == 1);
static_assert(__array_rank(Incomplete) == 0);
static_assert(__array_rank(Matrix*) == 0);
static_assert(__array_rank(int (&)[3]) == 0);
static_assert(__array_rank(void()) == 0);
static_assert(Rank<Matrix>::value == 3);
static_assert(RankAlias<Matrix>::value == 3);
static_assert(IntRank<Matrix>::value == 3);
static_assert(rank_v<int[2][3]> == 2);
static_assert(rank_v<Incomplete[]> == 1);

template<class T, int Rank = __array_rank(T)> struct DefaultRank : IntValue<Rank> {};
static_assert(DefaultRank<Matrix>::value == 3);

template<class T, bool Rank = __array_rank(T)> struct Fits {};
Fits<int[1]> accepted;

template<bool> using Discard = int;
template<class T> using DiscardedRank = Discard<__array_rank(T)>;
template<class T> using ForwardDiscardedRank = DiscardedRank<T>;
static_assert(__is_same(ForwardDiscardedRank<Incomplete[]>, int));

template<class T, bool N = __array_rank(T)> using Ranked = IntValue<N>;
template<class T> using ForwardRanked = Ranked<T>;
template<class T> using ForwardOverride = Ranked<T, false>;
static_assert(ForwardRanked<Incomplete[4]>::value == 1);
static_assert(ForwardRanked<int>::value == 0);
static_assert(ForwardOverride<Matrix>::value == 0);
