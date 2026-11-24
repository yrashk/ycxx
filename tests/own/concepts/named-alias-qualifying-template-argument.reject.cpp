// #1227: reject the qualifying alias template-id before expanding its type,
// even while recognizing an outer template-id ([temp.names], [temp.alias]).
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
Apply<names::NarrowScope<long>::template Pointer>::Type rejected = nullptr; //! error
