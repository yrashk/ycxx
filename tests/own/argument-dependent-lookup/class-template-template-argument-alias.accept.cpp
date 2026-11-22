// [basic.lookup.argdep], [temp.arg.template] (#165)
// XFAIL: clang,gcc Both oracles fail to find the function in the alias template's namespace.
template<template<class> class T> struct Carrier {};

namespace original {
template<class T> struct Template {};
}

namespace exposed {
template<class T> using Alias = original::Template<T>;
int score(const Carrier<Alias>&);
}

int call(const Carrier<exposed::Alias>& carrier) {
    return score(carrier);
}
