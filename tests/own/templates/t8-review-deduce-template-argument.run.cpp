// ζ t8 (#88, #208), from the step's review: a template template parameter is deduced from a template argument that is a
// template, in function templates and partial specializations ([temp.deduct.type]/8).
// EXIT: 121
template<class T> struct Box { T v; };
template<class T> struct Other { T v; };
template<template<class> class TT, class U> struct H {};
template<class T> struct S { static constexpr int n = 0; };
template<class T> struct S<H<Box, T>> { static constexpr int n = 1; };
template<template<class> class TT> struct S<H<TT, long>> { static constexpr int n = 2; };
template<class T> int f(H<Box, T>*) { return 1; }
int f(...) { return 0; }
int main() {
  return S<H<Box, int>>::n * 100 + S<H<Other, long>>::n * 10 + f((H<Box, int>*)0);
}
