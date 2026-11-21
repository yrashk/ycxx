// [basic.lookup.argdep] (#165)
template<template<class> class T> struct Holder {};

namespace providers {
template<class T> struct Provider {};
}

namespace unrelated {
int score(const Holder<providers::Provider>&);
}

int call(const Holder<providers::Provider>& holder) {
    unrelated::score(holder);
    return score(holder); //! error
}
