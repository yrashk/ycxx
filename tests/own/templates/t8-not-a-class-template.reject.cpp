// ζ t8 (#88, #208): a template template argument is a class or alias template, not a function
// template ([temp.arg.template]/1).
template<class T> int ft(T) { return 0; }
template<template<class> class TT> struct W {};
W<ft> w; //! error
int main() { return 0; }
