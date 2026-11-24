// A block-scope using-declaration takes precedence over an inherited non-template
// in both alias template uses ([basic.lookup.unqual]).
namespace N { template<class T> using Pick = T*; }
template<template<class> class F> struct Use { using Type = F<int>; };

struct Base { using Pick = char; };
struct Derived : Base {
  static int* select(int* value) {
    using N::Pick;
    Pick<int> direct = value;
    Use<Pick>::Type indirect = direct;
    return indirect;
  }
};

int main() {
  int value = 42;
  return Derived::select(&value) == &value ? 0 : 1;
}
