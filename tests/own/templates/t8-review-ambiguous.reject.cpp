// ζ t8 (#88, #208), from the step's review: inside B, `f<B>` fits both a function template taking a
// type and one taking a template ([temp.local]/1), so the call is ambiguous.
template<class T> int f() { return 1; }
template<template<class> class TT> int f() { return 2; }
template<class T> struct B { static int g() { return f<B>(); } }; //! error
int main() { return B<int>::g(); }
