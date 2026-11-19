// ζ t8 (#88, #208), from the step's review: a class template's injected-class-name is the class as a type argument and the
// template as a template argument ([temp.local]/1).
// EXIT: 0
template<class T> struct Holder { static constexpr int n = 5; };
template<class T> struct B {
  using H = Holder<B>;          // B is the injected-class-name as a type: Holder<B<T>>
  static constexpr int m = H::n;
  int get() { return Holder<B>::n; }
};
template<template<class> class TT> struct W { static constexpr int n = 7; };
template<class T> struct C {
  static constexpr int both = W<C>::n + Holder<C>::n;
};
int main() { B<int> b; return b.get() + B<char>::m + C<int>::both == 22 ? 0 : 1; }
