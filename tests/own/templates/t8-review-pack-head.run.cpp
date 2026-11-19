// ζ t8 (#88, #208), from the step's review: a trailing pack in a template template parameter's head
// takes every remaining parameter of the argument template ([temp.arg.template]/3's example).
// EXIT: 31
template <class T> struct eval { static constexpr int n = 0; };
template <template <class, class...> class TT, class T1, class... Rest>
struct eval<TT<T1, Rest...>> { static constexpr int n = 1 + sizeof...(Rest); };
template <class T1> struct A;
template <class T1, class T2> struct B;
template <int N> struct C;
template <class T1, int N> struct D;
template <class T1, class T2, int N = 17> struct E;
template<template<class...> class Q> struct Y { static constexpr int n = 5; };
int main() {
  int r = eval<A<int>>::n + eval<B<int, float>>::n * 10 + eval<C<17>>::n * 100 + eval<D<int, 17>>::n * 1000;
  return r + Y<A>::n + Y<B>::n;
}
