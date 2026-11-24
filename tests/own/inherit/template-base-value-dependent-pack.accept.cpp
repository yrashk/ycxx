// A pack expansion of value-dependent expressions in a class template's base argument
// ([temp.variadic], [temp.dep.constexpr]) (#308).
// XFAIL: ycxx needs a VDep* form for a value-dependent pack expansion in a base argument (#209)
template<int... V> struct PS { static constexpr int count = sizeof...(V); };
struct S { static constexpr int k = 9; };
template<class... T> struct Q : PS<T::k...> {};
template<int... N> struct R : PS<int(N + 1)...> {};
static_assert(Q<S, S>::count == 2 && R<1, 2, 3>::count == 3);
