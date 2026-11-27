// #1548: a value-dependent concept-id as a template argument in the nested-name-specifier of a
// typename-specifier, in an alias template and in a member alias of a class template
// ([temp.names]/8, [temp.dep.constexpr]/2, [temp.arg.nontype], [temp.res.general], [temp.alias]).
template<class T> concept HasDiff = requires { typename T::difference_type; };
template<class T> concept Small = sizeof(T) < 4;
template<bool B, class T, class F> struct cond_ { using type = T; };
template<class T, class F> struct cond_<false, T, F> { using type = F; };
template<bool B, class T, class F> using cond = typename cond_<B, T, F>::type;
struct A { using difference_type = long; };
struct Big { using difference_type = long; long pad; };
struct D1 { using difference_type = short; };
struct D2 { using difference_type = char; };
namespace N { template<class T> concept Wide = sizeof(T) >= 4; }

template<class I> using diff = typename cond<HasDiff<I>, D1, D2>::difference_type;
template<class I> using negated = typename cond_<!HasDiff<I>, D1, D2>::type::difference_type;
template<class I> using conjoined = typename cond_<HasDiff<I> && Small<I>, D1, D2>::type::difference_type;
template<class I> using qualified = typename cond<N::Wide<I>, D1, D2>::difference_type;
template<class I> using forwarded = diff<I>;
template<class I> using direct = cond<Small<I>, int, long>;
template<class I> struct S {
  using t = typename cond<HasDiff<I>, D1, D2>::difference_type;
  using u = cond<Small<I>, int, long>;
};

template<class, class> constexpr bool same = false;
template<class T> constexpr bool same<T, T> = true;
static_assert(same<diff<A>, short>);
static_assert(same<diff<int>, char>);
static_assert(same<negated<A>, char>);
static_assert(same<negated<int>, short>);
static_assert(same<conjoined<A>, short>);
static_assert(same<conjoined<Big>, char>);
static_assert(same<qualified<int>, short>);
static_assert(same<qualified<char>, char>);
static_assert(same<forwarded<A>, short>);
static_assert(same<forwarded<int>, char>);
static_assert(same<direct<char>, int>);
static_assert(same<direct<long>, long>);
static_assert(same<S<A>::t, short>);
static_assert(same<S<int>::t, char>);
static_assert(same<S<char>::u, int>);
static_assert(same<S<long>::u, long>);

template<class I> diff<I> distance(I) { return 0; }
static_assert(same<decltype(distance(A{})), short>);
static_assert(same<decltype(distance(0)), char>);
