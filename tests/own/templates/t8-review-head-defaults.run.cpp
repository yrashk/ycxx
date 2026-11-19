// ζ t8 (#88, #208), from the step's review: `TT<args>` takes the defaults of TT's own head, whatever
// template TT is bound to ([temp.arg.template], [temp.arg]/4).
// EXIT: 115
template<class T, class U> struct P2 { static constexpr int n = sizeof(U); };
template<class T, class U = char> struct P3 { static constexpr int n = sizeof(U); };
template<template<class, class = long> class TT> struct X { static constexpr int n = TT<char>::n; };
template<template<class...> class TT, class... Ts> struct Y { static constexpr int n = TT<int, Ts...>::n; };
template<class... Ts> struct Var { static constexpr int n = sizeof...(Ts); };
int main() {
  return X<P2>::n * 100 + X<P3>::n * 10 + Y<Var, char, long>::n;
}
