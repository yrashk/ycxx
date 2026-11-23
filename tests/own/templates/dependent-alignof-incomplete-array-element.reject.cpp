// [expr.alignof], [dcl.array]
struct Pending;
template<decltype(sizeof(0)) N> struct Value {};
template<class T> struct Alignment : Value<alignof(T)> {}; Alignment<Pending[]> instance; //! error
