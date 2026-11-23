// #1046 [meta.unary.prop] requires a complete class template specialization.
template<class T> struct Slot;
using Pending = Slot<int>;
constexpr bool is_sealed = __is_final(Pending); //! error
