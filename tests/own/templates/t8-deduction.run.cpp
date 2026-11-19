// ζ t8 (#88, #208): `TT<args>` deduces TT from a class template specialization, and its arguments
// from that template's specialization for them, defaults filled ([temp.deduct.type]/8); a partial
// specialization matches through a template template parameter.
// EXIT: 0
template<class T> struct Box { T v; };
template<class T, class U = int> struct V { T a; U b; };
template<template<class> class TT> int f(TT<int>*) { return 1; }
int f(...) { return 0; }
template<template<class...> class TT, class... Ts> int g(TT<Ts...>*) { return sizeof...(Ts); }
template<class T> struct W { static constexpr int n = 0; };
template<template<class> class TT, class U> struct W<TT<U>> { static constexpr int n = 1; };
int main() {
  if (f((Box<int>*)0) != 1 || f((V<int>*)0) != 1 || f((Box<char>*)0) != 0) return 1;
  if (g((V<int, char>*)0) != 2 || g((Box<long>*)0) != 1) return 2;
  return W<int>::n == 0 && W<Box<int>>::n == 1 && W<V<int>>::n == 1 ? 0 : 3;
}
