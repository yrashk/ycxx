// #1091: a non-dependent overflowing argument is invalid without template substitution
// ([expr.const.core], [temp.arg.nontype]).
template<int I>
constexpr int signed_index = I;

constexpr int invalid = signed_index<__INT_MAX__ - (-1)>; //! error
