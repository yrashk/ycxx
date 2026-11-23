// #1042 [meta.unary.prop] requires completeness when substituting the base argument.
struct Pending;

template<bool Value> struct FinalFlag {};
template<class T> struct FinalOf : FinalFlag<__is_final(T)> {}; //! error
struct Demand : FinalOf<const Pending> {};
