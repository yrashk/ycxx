// #1048 [meta.unary.prop] requires completeness for a class operand.
struct Pending;

template<bool Value> struct AggregateFlag {};
template<class T> struct Aggregate : AggregateFlag<__is_aggregate(T)> {}; //! error:52
struct Demand : Aggregate<const Pending> {};
