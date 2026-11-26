// [expr.prim.req.general], [expr.prim.req.type], [temp.alias]/2,3
using size_t = decltype(sizeof(0));

template<size_t N> struct Rep {};
template<> struct Rep<4> { using type = unsigned int; };

template<class T>
using AtomicRep = typename Rep<sizeof(T)>::type;

template<class T>
concept HasRep = requires { typename AtomicRep<T>; };

using FourBytes = unsigned char[4];
using ThreeBytes = unsigned char[3];
struct Incomplete;

static_assert(HasRep<FourBytes>);
static_assert(!HasRep<ThreeBytes>);
static_assert(!HasRep<Incomplete>);
