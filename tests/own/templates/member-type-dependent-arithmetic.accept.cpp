// [temp.dep.constexpr], [temp.arg.nontype], [temp.inst], [class.mem.general], [dcl.array]
template<int N> struct Buffer { int data[N]; };

template<int N> struct Wrap { Buffer<N + 1> storage; };

extern Wrap<2> small;
extern Wrap<7> large;
static_assert(sizeof(small.storage.data) == 3 * sizeof(int));
static_assert(sizeof(large.storage.data) == 8 * sizeof(int));
