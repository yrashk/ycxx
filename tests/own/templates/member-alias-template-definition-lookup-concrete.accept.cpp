// A nondependent base contributes its complete ancestry to member lookup
// ([class.member.lookup]), including bases that depended on its own parameters.
template<class> using Pick = int;
template<template<class> class F> struct Use { using type = F<int>; };

struct Base {
  template<class U> using Pick = U*;
};
template<class T> struct Fixed : T {};
template<class T> struct Derived : Fixed<Base> {
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
