// #1227: a satisfied namespace alias template-id can qualify a template
// template argument ([temp.names], [temp.alias], [temp.arg.template]).
struct Scope {
    template<class T> using Pointer = T*;
};

template<class T> struct SelectScope {
    static_assert(sizeof(T) == 1, "RHS expanded");
    using Type = Scope;
};

namespace names {
template<class T> requires (sizeof(T) == 1)
using NarrowScope = typename SelectScope<T>::Type;
}

template<template<class> class A> struct Apply { using Type = A<int>; };
Apply<names::NarrowScope<char>::template Pointer>::Type accepted = nullptr;
