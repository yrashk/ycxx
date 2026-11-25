// A qualified use selects the alias of the substituted base specialization
// ([temp.res.general], [temp.names], [temp.arg.template]).
template<class> using Pick = int;
template<template<class> class F> struct Use { using type = F<int>; };

template<class T> struct Base {
  template<class> using Pick = long long*;
};
template<> struct Base<int> {
  template<class> using Pick = int*;
};
template<class T> struct Derived : Base<T> {
  static int* direct(int* value) {
    typename Base<T>::template Pick<int> result = value;
    return result;
  }
  static int* argument(int* value) {
    typename Use<Base<T>::template Pick>::type result = value;
    return result;
  }
};

int* direct = Derived<int>::direct(nullptr);
int* argument = Derived<int>::argument(nullptr);
