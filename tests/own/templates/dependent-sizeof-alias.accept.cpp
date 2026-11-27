// [temp.dep.constexpr], [temp.arg.nontype]/2, [expr.sizeof], [temp.alias]/2,3
using size_t = decltype(sizeof(0));

template<size_t N> struct Rep {};
template<> struct Rep<1> { using type = unsigned char; };
template<> struct Rep<4> { using type = unsigned int; };

template<class T>
using AtomicRep = typename Rep<sizeof(T)>::type;

template<class, class>
constexpr bool same_type = false;

template<class T>
constexpr bool same_type<T, T> = true;

using FourBytes = unsigned char[4];
using Ordinary = typename Rep<sizeof(FourBytes)>::type;

static_assert(same_type<AtomicRep<unsigned char>, unsigned char>);
static_assert(same_type<AtomicRep<FourBytes>, unsigned int>);
static_assert(same_type<Ordinary, unsigned int>);
