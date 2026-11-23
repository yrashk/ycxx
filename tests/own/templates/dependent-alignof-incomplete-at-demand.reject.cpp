// [expr.alignof], [temp.point], [temp.inst]
struct Pending;
template<decltype(sizeof(0)) N> struct Value {};
template<class T> struct Alignment : Value<alignof(T)> {}; //! error
Alignment<Pending> instance; //! error
