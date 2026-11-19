// ζ t8 (#88): `__make_integer_seq<Seq, T, N>`, Clang's built-in template (docs/EXTENSIONS.md), is
// `Seq<T, 0, …, N-1>`; `__has_builtin` answers 1; libycxx's `make_integer_sequence` alias chain
// reaches it, and deduction reads through it.
// XFAIL: gcc GCC has no __make_integer_seq
// EXIT: 0
template<class T, T... Is> struct iseq { static constexpr int n = sizeof...(Is); static constexpr T sum = (T(0) + ... + Is); };
template<template<class U, U...> class Seq, class T, T N> using make_seq = __make_integer_seq<Seq, T, N>;
template<class T, T N> using make_integer_sequence = make_seq<iseq, T, N>;
template<unsigned long N> using make_index_sequence = make_integer_sequence<unsigned long, N>;
template<class T, T... Is> int count(iseq<T, Is...>*) { return sizeof...(Is); }
static_assert(make_index_sequence<4>::n == 4 && make_index_sequence<4>::sum == 6);
static_assert(make_integer_sequence<int, 0>::n == 0);
static_assert(__is_same(__make_integer_seq<iseq, char, 3>, iseq<char, 0, 1, 2>));
#if !__has_builtin(__make_integer_seq)
#error no
#endif
template<class T, T N> int h(make_integer_sequence<T, N>*) { return N; }
int main() { return count((make_index_sequence<5>*)0) == 5 && h<int, 2>(0) == 2 ? 0 : 1; }
