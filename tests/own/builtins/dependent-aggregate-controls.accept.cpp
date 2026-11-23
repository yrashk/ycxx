// #1048 [meta.unary.prop], [dcl.init.aggr].
template<class T>
inline constexpr bool aggregate_v = __is_aggregate(T);

struct Plain { int value; };
static_assert(aggregate_v<Plain>);
static_assert(!aggregate_v<int>);
static_assert(__is_aggregate(Plain));
static_assert(__is_aggregate(int[]));
static_assert(!__is_aggregate(int));
