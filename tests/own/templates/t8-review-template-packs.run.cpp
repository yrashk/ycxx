// ζ t8 (#88, #208), from the step's review: packs of template template parameters expand and are
// deduced, also from equal elements ([temp.variadic], [temp.deduct.type]/8).
// EXIT: 0
template<class T> struct Box {};
template<class T> struct Two {};
template<template<class> class... TTs> struct HP { static constexpr int n = sizeof...(TTs); };
template<template<class> class... TTs> struct P { using h = HP<TTs...>; using h2 = HP<Box, TTs..., Box>; };
template<template<class> class TT, template<class> class... TTs> struct Q { using h = HP<TTs..., TT>; };
static_assert(__is_same(P<Two, Box>::h, HP<Two, Box>));
static_assert(P<Two>::h2::n == 3);
static_assert(__is_same(P<Two>::h2, HP<Box, Two, Box>));
static_assert(__is_same(Q<Two, Box>::h, HP<Box, Two>));
static_assert(__is_same(P<>::h, HP<>));
template<class T> struct S { static constexpr int n = -1; };
template<template<class> class... TTs> struct S<HP<TTs...>> { static constexpr int n = sizeof...(TTs); };
template<template<class> class TT> struct S<HP<TT, TT>> { static constexpr int n = 100; };
static_assert(S<HP<Box, Two>>::n == 2);
static_assert(S<HP<Box, Box>>::n == 100);
int main() { return 0; }
