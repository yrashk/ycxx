// [basic.lookup.argdep], [temp.arg.template] (#165)
// EXIT: 0
template<template<class> class T> struct Holder { int value; };

namespace providers {
template<class T> struct Provider {};
int score(const Holder<Provider>& holder) { return holder.value + 1; }
}

int main() {
    Holder<providers::Provider> holder{41};
    if (providers::score(holder) != 42) return 1;
    if (score(holder) != 42) return 2;
    return 0;
}
