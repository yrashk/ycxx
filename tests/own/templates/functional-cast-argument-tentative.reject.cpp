// [dcl.ambig.res]/2 is syntactic: a template argument that parses as a type-id up to its end is
// one even when that type-id is ill-formed, though the same tokens are also a valid expression
// (#1190). `P(P(p))[1]` is a function returning an array ([dcl.fct]/11).
template<int N> struct IC { static constexpr int value = N; };
using P = const int*;
constexpr int arr[2] = {7, 8};
constexpr P p = arr;
constexpr int v = IC<P(P(p))[1]>::value; //! error
