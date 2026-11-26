// [temp.dep.constexpr], [temp.arg.nontype]/2, [expr.sizeof], [temp.alias]/2,3
using size_t = decltype(sizeof(0));

struct WideValue { unsigned char bytes[16]; };
struct WideRep { unsigned char bytes[16]; };
using EightBytes = unsigned char[8];

template<size_t N> struct ExplicitChoice {};
template<> struct ExplicitChoice<16> { using type = WideRep; };

template<size_t N> struct ConstrainedChoice { using type = EightBytes; };
template<size_t N> requires (N == 16)
struct ConstrainedChoice<N> { using type = WideRep; };

template<class T>
using ExplicitRep = typename ExplicitChoice<sizeof(T)>::type;

template<class T>
using ConstrainedRep = typename ConstrainedChoice<sizeof(T)>::type;

template<class, class>
constexpr bool same_type = false;

template<class T>
constexpr bool same_type<T, T> = true;

static_assert(sizeof(WideValue) == 16);
static_assert(same_type<ExplicitRep<WideValue>, WideRep>);
static_assert(same_type<ConstrainedRep<WideValue>, WideRep>);
static_assert(same_type<ConstrainedRep<EightBytes>, EightBytes>);
