// [expr.sizeof]/1, [temp.alias]/2,3
using size_t = decltype(sizeof(0));

template<size_t N> struct Rep { using type = int; };

template<class T>
using AtomicRep = typename Rep<sizeof(T)>::type; //! error

struct Incomplete;
using Invalid = AtomicRep<Incomplete>;
