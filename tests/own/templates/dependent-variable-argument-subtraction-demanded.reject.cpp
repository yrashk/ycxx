// #1091: the selected partial's formerly dependent argument overflows when demanded
// ([expr.const.core], [temp.arg.nontype], [temp.dep.constexpr], [temp.inst]).
template<int I>
constexpr int signed_index = I;
template<class T, int I>
constexpr int next = 7;
template<class T, int I>
constexpr int next<T*, I> = signed_index<I - (-1)>; //! error

constexpr int demanded = next<int*, __INT_MAX__>;
