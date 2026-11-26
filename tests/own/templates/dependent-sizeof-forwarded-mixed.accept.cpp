// [temp.dep.constexpr], [temp.arg.nontype]/2, [temp.alias]/2,3
using size_t = decltype(sizeof(0));

template<size_t Width, size_t Kind> struct Rep {};
template<> struct Rep<1, 2> { using type = short; };
template<> struct Rep<4, 2> { using type = long; };
template<> struct Rep<4, 3> { using type = char; };

template<class T, size_t Kind>
using AtomicRep = typename Rep<sizeof(T), Kind>::type;

template<class T, size_t Kind>
using Forwarded = AtomicRep<T, Kind>;

template<class, class>
constexpr bool same_type = false;

template<class T>
constexpr bool same_type<T, T> = true;

using FourBytes = unsigned char[4];

static_assert(same_type<Forwarded<unsigned char, 2>, short>);
static_assert(same_type<Forwarded<FourBytes, 2>, long>);
static_assert(same_type<Forwarded<FourBytes, 3>, char>);
