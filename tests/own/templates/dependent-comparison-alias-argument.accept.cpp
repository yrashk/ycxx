// #1548: a parenthesized value-dependent comparison as a template argument, in an alias template
// and in a member alias of a class template and of a partial specialization, read again when
// the arguments are known ([temp.dep.constexpr], [temp.arg.nontype], [temp.alias]/2).
using size_t = decltype(sizeof(0));
template<bool B, class T, class F> struct cond_ { using type = T; };
template<class T, class F> struct cond_<false, T, F> { using type = F; };
template<bool B, class T, class F> using cond = typename cond_<B, T, F>::type;
struct D1 { using difference_type = short; };
struct D2 { using difference_type = char; };

template<class I> using by_size = typename cond<(sizeof(I) > 4), D1, D2>::difference_type;
template<class I> using forwarded = by_size<I>;
template<class I> struct S { using u = typename cond<(sizeof(I) > 4), D1, D2>::difference_type; };
template<size_t, class...> struct element;
template<size_t I, class A, class B> struct element<I, A, B> { using type = cond<I == 0, A, B>; };

template<class, class> constexpr bool same = false;
template<class T> constexpr bool same<T, T> = true;
static_assert(same<by_size<long>, short>);
static_assert(same<by_size<char>, char>);
static_assert(same<forwarded<long>, short>);
static_assert(same<S<long>::u, short>);
static_assert(same<S<char>::u, char>);
static_assert(same<element<0, int, long>::type, int>);
static_assert(same<element<1, int, long>::type, long>);
