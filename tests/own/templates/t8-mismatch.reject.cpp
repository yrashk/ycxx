// ζ t8 (#88, #208): a template whose head takes two type arguments cannot be the argument of a
// template template parameter of one type parameter ([temp.arg.template]/4).
template<class T> struct Box {};
template<template<class, class> class TT> struct W {};
W<Box> w; //! error
int main() { return 0; }
