// #1244, #1371: template-argument identity includes the deduced type ([temp.type]/2,
// [temp.arg.nontype]/1): the `int` 1 and the `std::size_t` `sizeof(char)` name different
// specializations, although their values are equal.
template<class T, class U> constexpr bool same = false;
template<class T> constexpr bool same<T, T> = true;

template<auto N> constexpr unsigned deduced = 0;
template<> constexpr unsigned deduced<1> = 20;
template<> constexpr unsigned deduced<sizeof(char)> = 21;
static_assert(deduced<1> == 20);
static_assert(deduced<alignof(char)> == 21);
static_assert(deduced<1u> == 0);
static_assert(deduced<2> == 0);

template<auto X> struct cw { using type = decltype(X); };
static_assert(same<cw<3>::type, int>);
static_assert(same<cw<'a'>::type, char>);
static_assert(same<cw<3u>::type, unsigned>);
static_assert(same<cw<sizeof(int)>::type, decltype(sizeof(int))>);
static_assert(!same<cw<1>, cw<1u>>);
static_assert(!same<cw<1>, cw<true>>);
static_assert(same<cw<1>, cw<0 + 1>>);

// A dependent argument deduces its type once substituted. (A dependent layout query as the
// argument, `deduced<alignof(T)>` or `cw<sizeof(T)>`, is #1371's and #209's.)
template<class T, T V> struct Typed { using wrapped = cw<V>; static constexpr unsigned d = deduced<V>; };
static_assert(same<Typed<unsigned long, 1>::wrapped::type, unsigned long>);
static_assert(!same<Typed<unsigned long, 1>::wrapped, cw<1>>);
static_assert(Typed<int, 1>::d == 20 && Typed<decltype(sizeof(char)), 1>::d == 21 && Typed<unsigned, 1>::d == 0);
template<auto X> struct Outer { using inner = cw<X>; static constexpr unsigned d = deduced<X>; };
static_assert(same<Outer<'a'>::inner::type, char>);
static_assert(Outer<1>::d == 20 && Outer<sizeof(char)>::d == 21);
