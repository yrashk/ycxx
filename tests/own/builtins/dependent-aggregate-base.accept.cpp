// #1048 [meta.unary.prop], [dcl.init.aggr], [temp.arg.nontype].
template<bool Value>
struct AggregateFlag {
  static constexpr bool value = Value;
};

template<class T>
struct Aggregate : AggregateFlag<__is_aggregate(T)> {};

struct Plain { int value; };
struct Constructed {
  Constructed();
  int value;
};
struct Pending;
using OpenRows = int[][3];

static_assert(Aggregate<Plain>::value);
static_assert(!Aggregate<Constructed>::value);
static_assert(!Aggregate<int>::value);
static_assert(Aggregate<const Plain>::value);
static_assert(Aggregate<Constructed[2]>::value);
static_assert(Aggregate<const OpenRows>::value);
static_assert(Aggregate<Pending[2]>::value);
static_assert(!Aggregate<OpenRows&>::value);
static_assert(!Aggregate<const void>::value);
static_assert(Aggregate<Plain>::value);
