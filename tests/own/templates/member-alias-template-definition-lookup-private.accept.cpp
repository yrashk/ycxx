// A skipped dependent base contributes neither a template-id nor a template argument,
// even when finding its alias would make the use ill-formed ([temp.res.general]).
template<class> using Pick = int*;
template<template<class> class F> struct Use { using type = F<int>; };

template<class T> struct Base {
private:
  template<class> using Pick = long long*;
};
template<class T> struct Derived : Base<T> {
  static int* direct(int* value) {
    Pick<int> result = value;
    return result;
  }
  static int* argument(int* value) {
    typename Use<Pick>::type result = value;
    return result;
  }
};

int* direct = Derived<int>::direct(nullptr);
int* argument = Derived<int>::argument(nullptr);
