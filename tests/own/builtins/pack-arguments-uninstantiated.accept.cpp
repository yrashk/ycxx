// #1099: with a pack expansion among its arguments a builtin call's arity is not known until
// instantiation, so a pattern that no expansion could satisfy is not diagnosed in the definition.
template <class... A> int compare(A... a) { return __builtin_memcmp(a..., 1, 2, 3, 4); }
template <class... A> decltype(sizeof 0) length(A... a) { return __builtin_strlen("a", a...); }
template <class... A> void trap(A... a) { __builtin_trap(0, a...); }
